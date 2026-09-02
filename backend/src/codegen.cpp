#include "codegen.hpp"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include <iostream>
#include <map>
#include <stack>

CodeGenerator::CodeGenerator()
    : context(std::make_unique<llvm::LLVMContext>())
    , module(std::make_unique<llvm::Module>("aevix", *context))
    , builder(std::make_unique<llvm::IRBuilder<>>(*context))
{
    auto func_type = llvm::FunctionType::get(builder->getInt32Ty(), {}, false);
    main_func = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage, "main", module.get());
    entry_block = llvm::BasicBlock::Create(*context, "entry", main_func);
    builder->SetInsertPoint(entry_block);

    named_values.emplace_back();
    named_types.emplace_back();
}

void CodeGenerator::push_scope() {
    named_values.emplace_back();
    named_types.emplace_back();
}

void CodeGenerator::pop_scope() {
    named_values.pop_back();
    named_types.pop_back();
}

void CodeGenerator::define_var(const std::string& name, llvm::Value* alloc,
                               llvm::Type* ty) {
    named_values.back()[name] = alloc;
    named_types.back()[name] = ty;
}

llvm::Value* CodeGenerator::lookup_var(const std::string& name, llvm::Type*& ty) {
    for (auto it = named_values.rbegin(); it != named_values.rend(); ++it) {
        auto vit = it->find(name);
        if (vit != it->end()) {
            std::size_t idx = named_values.size() - 1 -
                              std::distance(named_values.rbegin(), it);
            ty = named_types[idx][name];
            return vit->second;
        }
    }
    return nullptr;
}

llvm::Value* CodeGenerator::generate_expr(const std::shared_ptr<Expr>& expr) {
    if (!expr) return nullptr;

    if (auto num = std::dynamic_pointer_cast<Number>(expr)) {
        return builder->getInt32(num->value);
    }
    else if (auto fl = std::dynamic_pointer_cast<Float>(expr)) {
        return llvm::ConstantFP::get(builder->getDoubleTy(), fl->value);
    }
    else if (auto b = std::dynamic_pointer_cast<Bool>(expr)) {
        return builder->getInt1(b->value ? 1 : 0);
    }
    else if (auto s = std::dynamic_pointer_cast<String>(expr)) {
        return builder->CreateGlobalString(s->value, "str");
    }
    else if (auto var = std::dynamic_pointer_cast<Variable>(expr)) {
        llvm::Type* ty = nullptr;
        llvm::Value* alloc = lookup_var(var->name, ty);
        if (alloc) {
            return builder->CreateLoad(ty, alloc, var->name);
        }
        std::cerr << "❌ Variable not found: " << var->name << std::endl;
        return nullptr;
    }
    else if (auto add = std::dynamic_pointer_cast<Add>(expr)) {
        auto left = generate_expr(add->left);
        auto right = generate_expr(add->right);
        if (!left || !right) return nullptr;
        if (left->getType()->isDoubleTy() || right->getType()->isDoubleTy()) {
            if (left->getType()->isIntegerTy(32)) left = builder->CreateSIToFP(left, builder->getDoubleTy());
            if (right->getType()->isIntegerTy(32)) right = builder->CreateSIToFP(right, builder->getDoubleTy());
            return builder->CreateFAdd(left, right, "addtmp");
        }
        return builder->CreateAdd(left, right, "addtmp");
    }
    else if (auto sub = std::dynamic_pointer_cast<Sub>(expr)) {
        auto left = generate_expr(sub->left);
        auto right = generate_expr(sub->right);
        if (!left || !right) return nullptr;
        if (left->getType()->isDoubleTy() || right->getType()->isDoubleTy()) {
            if (left->getType()->isIntegerTy(32)) left = builder->CreateSIToFP(left, builder->getDoubleTy());
            if (right->getType()->isIntegerTy(32)) right = builder->CreateSIToFP(right, builder->getDoubleTy());
            return builder->CreateFSub(left, right, "subtmp");
        }
        return builder->CreateSub(left, right, "subtmp");
    }
    else if (auto mul = std::dynamic_pointer_cast<Mul>(expr)) {
        auto left = generate_expr(mul->left);
        auto right = generate_expr(mul->right);
        if (!left || !right) return nullptr;
        if (left->getType()->isDoubleTy() || right->getType()->isDoubleTy()) {
            if (left->getType()->isIntegerTy(32)) left = builder->CreateSIToFP(left, builder->getDoubleTy());
            if (right->getType()->isIntegerTy(32)) right = builder->CreateSIToFP(right, builder->getDoubleTy());
            return builder->CreateFMul(left, right, "multmp");
        }
        return builder->CreateMul(left, right, "multmp");
    }
    else if (auto div = std::dynamic_pointer_cast<Div>(expr)) {
        auto left = generate_expr(div->left);
        auto right = generate_expr(div->right);
        if (!left || !right) return nullptr;
        if (left->getType()->isDoubleTy() || right->getType()->isDoubleTy()) {
            if (left->getType()->isIntegerTy(32)) left = builder->CreateSIToFP(left, builder->getDoubleTy());
            if (right->getType()->isIntegerTy(32)) right = builder->CreateSIToFP(right, builder->getDoubleTy());
            return builder->CreateFDiv(left, right, "divtmp");
        }
        return builder->CreateSDiv(left, right, "divtmp");
    }
    else if (auto neg = std::dynamic_pointer_cast<Neg>(expr)) {
        auto val = generate_expr(neg->value);
        if (!val) return nullptr;
        if (val->getType()->isDoubleTy()) return builder->CreateFNeg(val, "negtmp");
        return builder->CreateNeg(val, "negtmp");
    }
    else if (auto cmp = std::dynamic_pointer_cast<CmpOp>(expr)) {
        auto left = generate_expr(cmp->left);
        auto right = generate_expr(cmp->right);
        if (!left || !right) return nullptr;
        if (left->getType()->isDoubleTy() || right->getType()->isDoubleTy()) {
            if (left->getType()->isIntegerTy(32)) left = builder->CreateSIToFP(left, builder->getDoubleTy());
            if (right->getType()->isIntegerTy(32)) right = builder->CreateSIToFP(right, builder->getDoubleTy());
            return generate_fcmp(cmp->op, left, right);
        }
        if (left->getType()->isIntegerTy(1) || right->getType()->isIntegerTy(1)) {
            left = builder->CreateZExt(left, builder->getInt32Ty());
            right = builder->CreateZExt(right, builder->getInt32Ty());
            return generate_icmp(cmp->op, left, right);
        }
        return generate_icmp(cmp->op, left, right);
    }

    return nullptr;
}

llvm::Value* CodeGenerator::generate_icmp(const std::string& op,
                                          llvm::Value* left, llvm::Value* right) {
    if (op == "==") return builder->CreateICmpEQ(left, right, "cmptmp");
    if (op == "!=") return builder->CreateICmpNE(left, right, "cmptmp");
    if (op == "<")  return builder->CreateICmpSLT(left, right, "cmptmp");
    if (op == ">")  return builder->CreateICmpSGT(left, right, "cmptmp");
    if (op == "<=") return builder->CreateICmpSLE(left, right, "cmptmp");
    if (op == ">=") return builder->CreateICmpSGE(left, right, "cmptmp");
    std::cerr << "❌ Unknown icmp op: " << op << std::endl;
    return nullptr;
}

llvm::Value* CodeGenerator::generate_fcmp(const std::string& op,
                                          llvm::Value* left, llvm::Value* right) {
    if (op == "==") return builder->CreateFCmpOEQ(left, right, "cmptmp");
    if (op == "!=") return builder->CreateFCmpONE(left, right, "cmptmp");
    if (op == "<")  return builder->CreateFCmpOLT(left, right, "cmptmp");
    if (op == ">")  return builder->CreateFCmpOGT(left, right, "cmptmp");
    if (op == "<=") return builder->CreateFCmpOLE(left, right, "cmptmp");
    if (op == ">=") return builder->CreateFCmpOGE(left, right, "cmptmp");
    std::cerr << "❌ Unknown fcmp op: " << op << std::endl;
    return nullptr;
}

void CodeGenerator::generate_let(const Let& let) {
    llvm::Value* val = generate_expr(let.value);
    if (!val) return;

    llvm::Type* ty = val->getType();
    auto alloca = builder->CreateAlloca(ty, nullptr, let.name);
    builder->CreateStore(val, alloca);
    define_var(let.name, alloca, ty);
}

void CodeGenerator::generate_hot(const Hot& hot) {
    for (const auto& let : hot.body) {
        generate_let(let);
    }
}

void CodeGenerator::generate_print(const Print& print) {
    auto val = generate_expr(print.value);
    if (!val) return;

    llvm::Value* format_str = nullptr;
    llvm::Value* arg = val;

    if (val->getType()->isIntegerTy(32)) {
        format_str = builder->CreateGlobalString("%d\n", "format");
    }
    else if (val->getType()->isDoubleTy()) {
        format_str = builder->CreateGlobalString("%f\n", "format");
    }
    else if (val->getType()->isIntegerTy(1)) {
        format_str = builder->CreateGlobalString("%d\n", "format");
        arg = builder->CreateZExt(val, builder->getInt32Ty());
    }
    else if (val->getType()->isPointerTy()) {
        format_str = builder->CreateGlobalString("%s\n", "format");
    }
    else {
        return;
    }

    auto printf_type = llvm::FunctionType::get(
        builder->getInt32Ty(),
        llvm::PointerType::get(builder->getInt8Ty(), 0),
        true
    );
    auto printf_func = module->getOrInsertFunction("printf", printf_type);

    builder->CreateCall(printf_func, {format_str, arg});
}

void CodeGenerator::generate_block(const std::shared_ptr<Block>& block) {
    if (!block) return;
    push_scope();
    for (const auto& stmt : block->body) {
        generate_stmt(stmt);
    }
    pop_scope();
}

bool CodeGenerator::block_has_terminator(llvm::BasicBlock* bb) {
    if (bb->empty()) return false;
    return bb->back().isTerminator();
}

void CodeGenerator::generate_if(const If& if_stmt) {
    auto cond = generate_expr(if_stmt.condition);
    if (!cond) return;

    auto current = builder->GetInsertBlock();
    if (current->getTerminatorOrNull()) return;

    llvm::Function* func = current->getParent();
    auto then_block = llvm::BasicBlock::Create(*context, "then", func);
    auto else_block = llvm::BasicBlock::Create(*context, "else", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "merge");

    builder->CreateCondBr(cond, then_block, else_block);

    builder->SetInsertPoint(then_block);
    generate_block(if_stmt.then_block);
    if (!block_has_terminator(builder->GetInsertBlock())) {
        builder->CreateBr(merge_block);
    }

    builder->SetInsertPoint(else_block);
    generate_block(if_stmt.else_block);
    if (!block_has_terminator(builder->GetInsertBlock())) {
        builder->CreateBr(merge_block);
    }

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
}

void CodeGenerator::generate_stmt(const std::shared_ptr<Stmt>& stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case Stmt::Kind::Let:   generate_let(stmt->let); break;
        case Stmt::Kind::Hot:   generate_hot(stmt->hot); break;
        case Stmt::Kind::Print: generate_print(stmt->print); break;
        case Stmt::Kind::If:
            if (stmt->if_stmt) generate_if(*stmt->if_stmt);
            break;
    }
}

void CodeGenerator::finalize() {
    builder->CreateRet(builder->getInt32(0));

    llvm::verifyFunction(*main_func, &llvm::errs());

    module->print(llvm::outs(), nullptr);
}

void CodeGenerator::generate_program(const Program& program) {
    for (const auto& stmt : program.body) {
        generate_stmt(stmt);
    }
}
