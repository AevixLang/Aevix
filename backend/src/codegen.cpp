// ============================================================================
// Aevix backend — LLVM IR code generation
//
// Walks the AST produced by json_reader and emits LLVM IR for the "aevix"
// module. The variable model uses allocas + per-block scope stacks, so loops
// and branches require no PHI nodes (values are loaded/stored through allocas).
// ============================================================================
#include "codegen.hpp"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include <iostream>
#include <map>
#include <stack>

// ============================================================================
// Lifecycle: module setup + variable scope management
// ============================================================================

CodeGenerator::CodeGenerator()
    : context(std::make_unique<llvm::LLVMContext>())
    , module(std::make_unique<llvm::Module>("aevix", *context))
    , builder(std::make_unique<llvm::IRBuilder<>>(*context))
{
    auto func_type = llvm::FunctionType::get(builder->getInt32Ty(), {}, false);
    main_func = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage, "main", module.get());
    main_entry_block = llvm::BasicBlock::Create(*context, "entry", main_func);
    builder->SetInsertPoint(main_entry_block);

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

llvm::Type* CodeGenerator::llvm_type_for(const std::string& tn) {
    // Aevix type name -> LLVM type. Returns nullptr for "auto"/unknown types.
    if (tn == "int") return builder->getInt32Ty();
    if (tn == "float") return builder->getDoubleTy();
    if (tn == "bool") return builder->getInt1Ty();
    if (tn == "string") return llvm::PointerType::getUnqual(*context);
    return nullptr;
}

// ============================================================================
// Expressions: literals, arithmetic, comparisons, logical ops, function calls
// ============================================================================

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
    else if (auto call = std::dynamic_pointer_cast<Call>(expr)) {
        auto it = functions.find(call->callee);
        if (it == functions.end()) {
            std::cerr << "❌ Unknown function: " << call->callee << std::endl;
            return nullptr;
        }
        std::vector<llvm::Value*> args;
        for (auto& a : call->args) {
            auto v = generate_expr(a);
            if (!v) return nullptr;
            args.push_back(v);
        }
        if (it->second->getReturnType()->isVoidTy()) {
            builder->CreateCall(it->second, args);
            return nullptr;
        }
        return builder->CreateCall(it->second, args, "calltmp");
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
    else if (auto not_ = std::dynamic_pointer_cast<Not>(expr)) {
        auto val = generate_expr(not_->value);
        if (!val) return nullptr;
        return builder->CreateXor(val, builder->getInt1(true), "nottmp");
    }
    else if (auto and_ = std::dynamic_pointer_cast<And>(expr)) {
        return generate_logical_and(*and_);
    }
    else if (auto or_ = std::dynamic_pointer_cast<Or>(expr)) {
        return generate_logical_or(*or_);
    }

    return nullptr;
}

// Short-circuit evaluation of && / || via branching + PHI merging.
// Each contains its own section but they form a single logical unit.
llvm::Value* CodeGenerator::generate_logical_and(const And& and_) {
    auto lhs = generate_expr(and_.left);
    if (!lhs) return nullptr;

    auto current = builder->GetInsertBlock();
    llvm::Function* func = current->getParent();
    auto rhs_block = llvm::BasicBlock::Create(*context, "and.rhs", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "and.merge", func);

    builder->CreateCondBr(lhs, rhs_block, merge_block);

    builder->SetInsertPoint(rhs_block);
    auto rhs = generate_expr(and_.right);
    if (!rhs) return nullptr;
    if (!block_has_terminator(builder->GetInsertBlock())) {
        builder->CreateBr(merge_block);
    }

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
    llvm::PHINode* phi = builder->CreatePHI(builder->getInt1Ty(), 2, "andtmp");
    phi->addIncoming(builder->getFalse(), current);
    phi->addIncoming(rhs, rhs_block);
    return phi;
}

llvm::Value* CodeGenerator::generate_logical_or(const Or& or_) {
    auto lhs = generate_expr(or_.left);
    if (!lhs) return nullptr;

    auto current = builder->GetInsertBlock();
    llvm::Function* func = current->getParent();
    auto rhs_block = llvm::BasicBlock::Create(*context, "or.rhs", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "or.merge", func);

    builder->CreateCondBr(lhs, merge_block, rhs_block);

    builder->SetInsertPoint(rhs_block);
    auto rhs = generate_expr(or_.right);
    if (!rhs) return nullptr;
    if (!block_has_terminator(builder->GetInsertBlock())) {
        builder->CreateBr(merge_block);
    }

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
    llvm::PHINode* phi = builder->CreatePHI(builder->getInt1Ty(), 2, "ortmp");
    phi->addIncoming(builder->getTrue(), current);
    phi->addIncoming(rhs, rhs_block);
    return phi;
}

// Signed integer comparison helper (shared by ==, !=, <, >, <=, >=)
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

// Ordered floating-point comparison helper (shared by ==, !=, <, >, <=, >=)
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

// ============================================================================
// Statements: declarations, IO, control flow, assignments
// ============================================================================

void CodeGenerator::generate_let(const Let& let) {
    llvm::Value* val = generate_expr(let.value);
    if (!val) return;

    llvm::Type* ty = val->getType();
    auto alloca = builder->CreateAlloca(ty, nullptr, let.name);
    builder->CreateStore(val, alloca);
    define_var(let.name, alloca, ty);
}

void CodeGenerator::generate_assign(const Assign& a) {
    llvm::Type* ty = nullptr;
    llvm::Value* alloc = lookup_var(a.name, ty);
    if (!alloc) {
        std::cerr << "❌ Cannot assign to unknown variable: " << a.name << std::endl;
        return;
    }
    llvm::Value* val = generate_expr(a.value);
    if (!val) return;
    builder->CreateStore(val, alloc);
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
        llvm::PointerType::getUnqual(*context),
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

// while: entry -> cond -> body -> back to cond ; cond -> end on false
void CodeGenerator::generate_while(const While& w) {
    auto func = builder->GetInsertBlock()->getParent();

    auto cond_block = llvm::BasicBlock::Create(*context, "while.cond", func);
    auto body_block = llvm::BasicBlock::Create(*context, "while.body", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "while.end");

    builder->CreateBr(cond_block);
    builder->SetInsertPoint(cond_block);
    auto cond = generate_expr(w.condition);
    if (!cond) return;
    builder->CreateCondBr(cond, body_block, merge_block);

    builder->SetInsertPoint(body_block);
    generate_block(w.body);
    if (!block_has_terminator(builder->GetInsertBlock())) {
        builder->CreateBr(cond_block);
    }

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
}

// C-style for(init; cond; step): entry -> cond -> body -> step -> back to cond
void CodeGenerator::generate_for(const For& f) {
    auto func = builder->GetInsertBlock()->getParent();

    if (f.init) {
        generate_stmt(f.init);
    }

    auto cond_block = llvm::BasicBlock::Create(*context, "for.cond", func);
    auto body_block = llvm::BasicBlock::Create(*context, "for.body", func);
    auto step_block = llvm::BasicBlock::Create(*context, "for.step", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "for.end");

    builder->CreateBr(cond_block);

    builder->SetInsertPoint(cond_block);
    if (f.condition) {
        auto cond = generate_expr(f.condition);
        if (!cond) return;
        builder->CreateCondBr(cond, body_block, merge_block);
    } else {
        builder->CreateBr(body_block);
    }

    builder->SetInsertPoint(body_block);
    generate_block(f.body);
    if (!block_has_terminator(builder->GetInsertBlock())) {
        builder->CreateBr(step_block);
    }

    builder->SetInsertPoint(step_block);
    if (f.step) {
        generate_stmt(f.step);
    }
    if (!block_has_terminator(builder->GetInsertBlock())) {
        builder->CreateBr(cond_block);
    }

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
}

void CodeGenerator::generate_return(const Return& r) {
    llvm::Type* ret_ty = return_type_stack.empty() ? nullptr : return_type_stack.back();
    if (r.value) {
        auto val = generate_expr(r.value);
        if (!val) return;
        if (ret_ty && ret_ty->isDoubleTy() && val->getType()->isIntegerTy(32)) {
            val = builder->CreateSIToFP(val, builder->getDoubleTy());
        }
        builder->CreateRet(val);
    } else {
        if (ret_ty && !ret_ty->isVoidTy()) {
            if (ret_ty->isIntegerTy(32)) builder->CreateRet(builder->getInt32(0));
            else if (ret_ty->isDoubleTy()) builder->CreateRet(llvm::ConstantFP::get(builder->getDoubleTy(), 0.0));
            else if (ret_ty->isIntegerTy(1)) builder->CreateRet(builder->getInt1(false));
            else builder->CreateRet(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ret_ty)));
        } else {
            builder->CreateRetVoid();
        }
    }
}

// ============================================================================
// Functions: prototype declaration + body generation
// ============================================================================

// Declaration half: create the hollow skeleton so forward references inside any
// body resolve before any body is emitted.
void CodeGenerator::declare_func(const FuncDecl& fd) {
    std::vector<llvm::Type*> param_types;
    for (auto& p : fd.params) {
        llvm::Type* t = llvm_type_for(p.var_type);
        if (!t) t = builder->getInt32Ty();
        param_types.push_back(t);
    }
    llvm::Type* ret_ty = llvm_type_for(fd.return_type);
    if (!ret_ty) ret_ty = builder->getVoidTy();   // unspecified -> void

    auto func_type = llvm::FunctionType::get(ret_ty, param_types, false);
    auto func = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage, fd.name, module.get());
    functions[fd.name] = func;
}

// Body half: fill the skeleton — entry block, params as allocas, then body.
void CodeGenerator::generate_func_decl(const FuncDecl& fd) {
    auto it = functions.find(fd.name);
    if (it == functions.end()) return;
    llvm::Function* func = it->second;

    auto entry = llvm::BasicBlock::Create(*context, "entry", func);
    builder->SetInsertPoint(entry);

    push_scope();
    return_type_stack.push_back(func->getReturnType()->isVoidTy() ? nullptr : func->getReturnType());

    // Copy each argument into a named alloca so params behave like locals
    // (reads/writes always go through the alloca, matching the variable model).
    auto arg_it = func->arg_begin();
    for (auto& p : fd.params) {
        llvm::Argument& arg = *arg_it++;
        llvm::Type* pt = llvm_type_for(p.var_type);
        if (!pt) pt = builder->getInt32Ty();
        auto alloca = builder->CreateAlloca(pt, nullptr, p.name);
        builder->CreateStore(&arg, alloca);
        define_var(p.name, alloca, pt);
    }

    // Emit the function body, then add an implicit return if it ends without
    // a terminator (e.g. a function with a code path missing an explicit ret).
    generate_block(fd.body);
    if (!block_has_terminator(builder->GetInsertBlock())) {
        llvm::Type* ret_ty = func->getReturnType();
        if (ret_ty->isVoidTy()) builder->CreateRetVoid();
        else if (ret_ty->isIntegerTy(32)) builder->CreateRet(builder->getInt32(0));
        else if (ret_ty->isDoubleTy()) builder->CreateRet(llvm::ConstantFP::get(builder->getDoubleTy(), 0.0));
        else if (ret_ty->isIntegerTy(1)) builder->CreateRet(builder->getInt1(false));
        else builder->CreateRet(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ret_ty)));
    }

    return_type_stack.pop_back();
    pop_scope();
}

// ============================================================================
// Statement dispatch + final output
// ============================================================================

// Route one parsed statement to its dedicated generator.
void CodeGenerator::generate_stmt(const std::shared_ptr<Stmt>& stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case Stmt::Kind::Let:      generate_let(stmt->let); break;
        case Stmt::Kind::Hot:      generate_hot(stmt->hot); break;
        case Stmt::Kind::Print:    generate_print(stmt->print); break;
        case Stmt::Kind::If:       if (stmt->if_stmt) generate_if(*stmt->if_stmt); break;
        case Stmt::Kind::While:    if (stmt->while_stmt) generate_while(*stmt->while_stmt); break;
        case Stmt::Kind::For:      if (stmt->for_stmt) generate_for(*stmt->for_stmt); break;
        case Stmt::Kind::Return:   if (stmt->return_stmt) generate_return(*stmt->return_stmt); break;
        case Stmt::Kind::Assign:   if (stmt->assign_stmt) generate_assign(*stmt->assign_stmt); break;
        case Stmt::Kind::FuncDecl: generate_func_decl(*stmt->func_decl); break;
        case Stmt::Kind::CallStmt: if (stmt->call_stmt) generate_expr(stmt->call_stmt); break;
    }
}

// True if a block already ends in a terminator (return/branch), so we must not
// append another one after it.
bool CodeGenerator::block_has_terminator(llvm::BasicBlock* bb) {
    if (bb->empty()) return false;
    return bb->back().isTerminator();
}

// Close out the current block, run LLVM's verifier, and print the IR to stdout.
void CodeGenerator::finalize() {
    auto cur = builder->GetInsertBlock();
    if (cur && !cur->getTerminatorOrNull()) {
        builder->CreateRet(builder->getInt32(0));
    }

    llvm::verifyFunction(*main_func, &llvm::errs());

    module->print(llvm::outs(), nullptr);
}

// ============================================================================
// Program driver: multi-phase generation so functions can call each other
// ============================================================================

void CodeGenerator::generate_program(const Program& program) {
    // Phase 1 — declare every user function as a hollow prototype. Doing this
    // before any bodies are emitted means functions may reference functions
    // declared later in the source.
    for (const auto& stmt : program.body) {
        if (stmt->kind == Stmt::Kind::FuncDecl && stmt->func_decl) {
            declare_func(*stmt->func_decl);
        }
    }

    // Phase 2 — emit the body of every user function.
    for (const auto& stmt : program.body) {
        if (stmt->kind == Stmt::Kind::FuncDecl && stmt->func_decl) {
            generate_func_decl(*stmt->func_decl);
        }
    }

    // Phase 3 — reset insertion to main and emit all remaining top-level
    // statements (the implicit "main" body).
    builder->SetInsertPoint(main_entry_block);
    for (const auto& stmt : program.body) {
        if (stmt->kind != Stmt::Kind::FuncDecl) {
            generate_stmt(stmt);
        }
    }
}