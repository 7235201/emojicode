//
//  MFFunctionAnalyser.cpp
//  EmojicodeCompiler
//
//  Created by Theo Weidmann on 01/06/2018.
//  Copyright © 2018 Theo Weidmann. All rights reserved.
//

#include "MFFunctionAnalyser.hpp"
#include "AST/ASTExpr.hpp"
#include "AST/ASTLiterals.hpp"
#include "AST/ASTMemory.hpp"
#include "AST/ASTStatements.hpp"
#include "AST/ASTVariables.hpp"
#include "AST/Releasing.hpp"
#include "Functions/Function.hpp"
#include "MFHeapAllocates.hpp"
#include "Scoping/SemanticScopeStats.hpp"

namespace EmojicodeCompiler {

const MFFlowCategory MFFlowCategory::Borrowing = MFFlowCategory::Category::Borrowing;
const MFFlowCategory MFFlowCategory::Escaping = MFFlowCategory::Category::Escaping;
const MFFlowCategory MFFlowCategory::Return = MFFlowCategory::Category::Return;

MFFunctionAnalyser::MFFunctionAnalyser(Function *function) : scope_(function->variableCount()), function_(function) {}

void MFFunctionAnalyser::analyse() {
    if (!function_->memoryFlowTypeForThis().isUnknown()) {
        return;
    }

    function_->setMemoryFlowTypeForThis(MFFlowCategory::Escaping);

    for (size_t i = 0; i < function_->parameters().size(); i++) {
        auto &var = scope_.getVariable(i);
        var.isParam = true;
        var.param = i;
    }

    function_->ast()->analyseMemoryFlow(this);
    function_->setMemoryFlowTypeForThis(thisEscapes_ ? MFFlowCategory::Escaping : MFFlowCategory::Borrowing);
    popScope(function_->ast());
}

void MFFunctionAnalyser::analyseFunctionCall(ASTArguments *node, ASTExpr *callee, Function *function) {
    if (function->memoryFlowTypeForThis().isUnknown()) {
        MFFunctionAnalyser(function).analyse();
    }
    if (callee != nullptr) {
        callee->analyseMemoryFlow(this, function->memoryFlowTypeForThis());
    }
    for (size_t i = 0; i < node->args().size(); i++) {
        node->args()[i]->analyseMemoryFlow(this, function->parameters()[i].memoryFlowType);
    }
}

void MFFunctionAnalyser::popScope(ASTBlock *block) {
    releaseVariables(block);

    for (size_t i = 0; i < block->scopeStats().variables; i++) {
        auto &var = scope_.getVariable(i + block->scopeStats().from);
        if (var.isParam) {
            function_->setParameterMFType(var.param, var.flowCategory);
        }
        else if (!var.flowCategory.isEscaping()) {
            for (auto init : var.inits) {
                init->allocateOnStack();
            }
        }
        var.inits.clear();
    }
}

bool MFFunctionAnalyser::shouldReleaseVariable(const MFLocalVariable &var) const {
    return !var.isParam && !var.isReturned && var.type.isManaged();
}

void MFFunctionAnalyser::releaseVariables(ASTBlock *block) const {
    // If this block does not return certainly, we can simply release its local variables at the end of the block.
    if (!block->returnedCertainly()) {
        for (size_t i = 0; i < block->scopeStats().variables; i++) {
            VariableID variableId = i + block->scopeStats().from;
            auto &var = scope_.getVariable(variableId);
            if (shouldReleaseVariable(var)) {
                block->appendNode(std::make_unique<ASTRelease>(false, variableId, var.type, block->position()));
            }
        }
    }
    // Otherwise, determine whether the last statement of the block is a return statement.
    // If it is a return statement, add the release statements to the return statement.
    else if (auto returnStmt = block->getReturn()) {
        releaseAllVariables(returnStmt, block->scopeStats(), block->position());
    }
}

void MFFunctionAnalyser::releaseAllVariables(Releasing *releasing, const SemanticScopeStats &stats,
                                             const SourcePosition &p) const {
    for (size_t i = 0; i < stats.allVariablesCount; i++) {
        VariableID variableId = i;
        auto &var = scope_.getVariable(variableId);
        if (shouldReleaseVariable(var)) {
            releasing->addRelease(std::make_unique<ASTRelease>(false, variableId, var.type, p));
        }
    }
}

void MFFunctionAnalyser::recordVariableGet(size_t id, MFFlowCategory category) {
    if (category.isReturn()) {
        auto &var = scope_.getVariable(id);
        if (var.isParam) return;
        var.isReturned = true;
    }
    if (category.isEscaping()) {
        scope_.getVariable(id).flowCategory = category;
    }
}

void MFFunctionAnalyser::take(ASTExpr *expr) {
    expr->unsetIsTemporary();
}

void MFFunctionAnalyser::recordThis(MFFlowCategory category) {
    if (category.isEscaping() && !category.isReturn()) {
        thisEscapes_ = true;
    }
}

void MFFunctionAnalyser::recordVariableSet(size_t id, ASTExpr *expr, Type type) {
    auto &var = scope_.getVariable(id);
    var.type = std::move(type);
    if (expr != nullptr) {
        expr->analyseMemoryFlow(this, MFFlowCategory::Escaping);
        if (auto heapAllocates = dynamic_cast<MFHeapAllocates *>(expr)) {
            var.inits.emplace_back(heapAllocates);
        }
    }
}

}  // namespace EmojicodeCompiler
