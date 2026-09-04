// ============================================================================
// Aevix Backend: LLVM IR Code Generation
//
// This module handles the transformation of the Aevix AST into LLVM Intermediate
// Representation (IR). It manages variable scoping, type checking, and emits
// the final machine-executable code.
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
// Lifecycle: Module Setup & Variable Scope Management
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

    build_oob_runtime();

    named_values.emplace_back();
    named_types.emplace_back();
}

/**
 * Builds the runtime array bounds-check helper.
 * Prints an error message and exits(1) on out-of-bounds access.
 */
void CodeGenerator::build_oob_runtime() {
    auto i32 = builder->getInt32Ty();
    auto void_ty = llvm::Type::getVoidTy(*context);
    auto ptr_ty = llvm::PointerType::getUnqual(*context);

    llvm::FunctionType* ft = llvm::FunctionType::get(void_ty, {i32, i32}, false);
    oob_func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "__aevix_oob", module.get());

    auto body = llvm::BasicBlock::Create(*context, "entry", oob_func);
    llvm::IRBuilder<> tmp(*context);
    tmp.SetInsertPoint(body);

    auto fmt = tmp.CreateGlobalString("array index %d out of bounds (size %d)\n", "oob_fmt");

    llvm::FunctionType* printf_ft = llvm::FunctionType::get(i32, {ptr_ty}, true);
    module->getOrInsertFunction("printf", printf_ft);
    auto printf_fn = module->getFunction("printf");
    tmp.CreateCall(printf_fn, {fmt, oob_func->arg_begin(), oob_func->arg_begin() + 1});

    llvm::FunctionType* exit_ft = llvm::FunctionType::get(void_ty, {i32}, false);
    module->getOrInsertFunction("exit", exit_ft);
    auto exit_fn = module->getFunction("exit");
    exit_fn->addFnAttr(llvm::Attribute::NoReturn);
    tmp.CreateCall(exit_fn, {tmp.getInt32(1)});
    tmp.CreateUnreachable();
}

/**
 * Throws a runtime error to stop IR emission upon discovering a semantic fault.
 */
void CodeGenerator::error(const std::string& msg) {
    throw std::runtime_error(msg);
}

void CodeGenerator::push_scope() {
    named_values.emplace_back();
    named_types.emplace_back();
}

void CodeGenerator::pop_scope() {
    named_values.pop_back();
    named_types.pop_back();
}

void CodeGenerator::define_var(const std::string& name, llvm::Value* alloc, llvm::Type* ty) {
    named_values.back()[name] = alloc;
    named_types.back()[name] = ty;
}

llvm::Value* CodeGenerator::lookup_var(const std::string& name, llvm::Type*& ty) {
    for (auto it = named_values.rbegin(); it != named_values.rend(); ++it) {
        auto vit = it->find(name);
        if (vit != it->end()) {
            std::size_t idx = named_values.size() - 1 - std::distance(named_values.rbegin(), it);
            ty = named_types[idx][name];
            return vit->second;
        }
    }
    return nullptr;
}

/**
 * Parses an Aevix type string (e.g., "int[5]") into base type and array size.
 */
bool CodeGenerator::parse_type(const std::string& tn, std::string& base, int& arr_size) {
    base = tn;
    arr_size = -1;
    std::size_t br = tn.find('[');
    if (br != std::string::npos) {
        base = tn.substr(0, br);
        std::size_t close = tn.find(']', br);
        std::string sz = tn.substr(br + 1, close - br - 1);
        arr_size = sz.empty() ? 0 : std::stoi(sz);
    }
    return base == "int" || base == "float" || base == "bool" || base == "string";
}

llvm::Type* CodeGenerator::scalar_type_for(const std::string& base) {
    if (base == "int") return builder->getInt32Ty();
    if (base == "float") return builder->getDoubleTy();
    if (base == "bool") return builder->getInt1Ty();
    if (base == "string") return llvm::PointerType::getUnqual(*context);
    return nullptr;
}

llvm::Type* CodeGenerator::llvm_type_for(const std::string& tn) {
    std::string base;
    int arr_size = -1;
    if (!parse_type(tn, base, arr_size)) return nullptr;
    llvm::Type* b = scalar_type_for(base);
    if (!b) return nullptr;
    if (arr_size >= 0) {
        if (arr_size == 0) return nullptr;
        return llvm::ArrayType::get(b, arr_size);
    }
    return b;
}

llvm::Type* CodeGenerator::signature_type_for(const std::string& tn, const std::string& ctx) {
    if (tn.empty()) return nullptr;
    std::string base;
    int arr_size = -1;
    if (!parse_type(tn, base, arr_size)) {
        error("Unknown type '" + tn + "' in " + ctx);
    }
    llvm::Type* b = scalar_type_for(base);
    if (arr_size >= 0) {
        if (arr_size == 0) {
            error("Array type '" + tn + "' in " + ctx + " needs a fixed size");
        }
        return llvm::ArrayType::get(b, arr_size);
    }
    return b;
}

bool CodeGenerator::is_numeric(llvm::Type* ty) {
    return ty->isIntegerTy(32) || ty->isDoubleTy();
}

void CodeGenerator::require_numeric(llvm::Value* v, const std::string& ctx) {
    if (!is_numeric(v->getType())) {
        error("Operator " + ctx + " requires numeric operands");
    }
}

void CodeGenerator::require_bool(llvm::Value* v, const std::string& ctx) {
    require_bool_type(v->getType(), ctx);
}

void CodeGenerator::require_bool_type(llvm::Type* ty, const std::string& ctx) {
    if (!ty->isIntegerTy(1)) {
        error(ctx + " requires a bool condition");
    }
}

std::string CodeGenerator::llvm_type_name(llvm::Type* ty) {
    if (ty->isIntegerTy(32)) return "int";
    if (ty->isDoubleTy()) return "float";
    if (ty->isIntegerTy(1)) return "bool";
    if (ty->isPointerTy()) return "string";
    if (ty->isArrayTy()) return "array";
    return "unknown";
}

// ============================================================================
// Array Handling
// ============================================================================

llvm::Type* CodeGenerator::element_type_of(const std::shared_ptr<Expr>& e) {
    if (auto n = std::dynamic_pointer_cast<Number>(e)) return builder->getInt32Ty();
    if (auto f = std::dynamic_pointer_cast<Float>(e)) return builder->getDoubleTy();
    if (auto b = std::dynamic_pointer_cast<Bool>(e)) return builder->getInt1Ty();
    if (auto s = std::dynamic_pointer_cast<String>(e)) return llvm::PointerType::getUnqual(*context);
    if (auto al = std::dynamic_pointer_cast<ArrayLit>(e)) return build_array_type(*al);
    if (auto var = std::dynamic_pointer_cast<Variable>(e)) {
        llvm::Type* t = nullptr;
        llvm::Value* alloc = lookup_var(var->name, t);
        if (!alloc) error("Variable not found: " + var->name);
        if (!t->isArrayTy()) error("Variable '" + var->name + "' is not an array");
        return t;
    }
    error("Array elements must be literals of a single type");
}

llvm::Type* CodeGenerator::build_array_type(const ArrayLit& al) {
    if (al.elements.empty()) error("Cannot infer the element type of an empty array ([])");
    llvm::Type* elem = element_type_of(al.elements[0]);
    for (std::size_t i = 1; i < al.elements.size(); ++i) {
        llvm::Type* other = element_type_of(al.elements[i]);
        if (other != elem) {
            error("Array elements must share a single type (got " + llvm_type_name(elem) + " and " + llvm_type_name(other) + ")");
        }
    }
    return llvm::ArrayType::get(elem, al.elements.size());
}

llvm::Constant* CodeGenerator::build_array_constant(const ArrayLit& al) {
    llvm::Type* arr_ty = build_array_type(al);
    std::vector<llvm::Constant*> cels;
    cels.reserve(al.elements.size());
    for (const auto& e : al.elements) {
        if (auto n = std::dynamic_pointer_cast<Number>(e)) cels.push_back(builder->getInt32(n->value));
        else if (auto f = std::dynamic_pointer_cast<Float>(e)) cels.push_back(llvm::ConstantFP::get(builder->getDoubleTy(), f->value));
        else if (auto b = std::dynamic_pointer_cast<Bool>(e)) cels.push_back(builder->getInt1(b->value ? 1 : 0));
        else if (auto s = std::dynamic_pointer_cast<String>(e)) cels.push_back(builder->CreateGlobalString(s->value, "str"));
        else if (auto inner = std::dynamic_pointer_cast<ArrayLit>(e)) cels.push_back(build_array_constant(*inner));
        else error("Array elements must be literals of a single type");
    }
    return llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(arr_ty), cels);
}

llvm::Value* CodeGenerator::gen_index_ptr(const Index& top, llvm::Type*& elem_ty) {
    std::vector<const Index*> chain;
    const Expr* cur = &top;
    while (auto ix = dynamic_cast<const Index*>(cur)) {
        chain.push_back(ix);
        cur = ix->object.get();
    }
    auto var = dynamic_cast<const Variable*>(cur);
    if (!var) error("Invalid array access target");

    llvm::Type* arr_ty = nullptr;
    llvm::Value* alloc = lookup_var(var->name, arr_ty);
    if (!alloc) error("Variable not found: " + var->name);
    if (!arr_ty->isArrayTy()) error("Variable '" + var->name + "' is not an array");

    llvm::Value* ptr = alloc;
    llvm::Type* cur_arr = arr_ty;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!cur_arr->isArrayTy()) error("Indexing a non-array value");
        llvm::Value* idx = generate_expr((*it)->index);
        if (!idx) error("Invalid array index expression");
        if (!idx->getType()->isIntegerTy(32)) error("Array index must be an integer");
        auto dim = (int)cur_arr->getArrayNumElements();

        llvm::Value* neg = builder->CreateICmpSLT(idx, builder->getInt32(0));
        llvm::Value* huge = builder->CreateICmpSGE(idx, builder->getInt32(dim));
        llvm::Value* oob = builder->CreateOr(neg, huge, "oob");

        auto ok = llvm::BasicBlock::Create(*context, "idxok", builder->GetInsertBlock()->getParent());
        auto fail = llvm::BasicBlock::Create(*context, "idxfail", builder->GetInsertBlock()->getParent());
        builder->CreateCondBr(oob, fail, ok);
        builder->SetInsertPoint(fail);
        builder->CreateCall(oob_func, {idx, builder->getInt32(dim)});
        builder->CreateUnreachable();

        builder->SetInsertPoint(ok);
        ptr = builder->CreateInBoundsGEP(cur_arr, ptr, {builder->getInt32(0), idx}, "idx");
        cur_arr = cur_arr->getArrayElementType();
    }
    elem_ty = cur_arr;
    return ptr;
}

// ============================================================================
// Expressions: Literals, Arithmetic, Comparisons, Logic, Calls
// ============================================================================

llvm::Value* CodeGenerator::generate_expr(const std::shared_ptr<Expr>& expr) {
    if (!expr) return nullptr;

    if (auto num = std::dynamic_pointer_cast<Number>(expr)) return builder->getInt32(num->value);
    if (auto fl = std::dynamic_pointer_cast<Float>(expr)) return llvm::ConstantFP::get(builder->getDoubleTy(), fl->value);
    if (auto b = std::dynamic_pointer_cast<Bool>(expr)) return builder->getInt1(b->value ? 1 : 0);
    if (auto s = std::dynamic_pointer_cast<String>(expr)) return builder->CreateGlobalString(s->value, "str");
    if (auto var = std::dynamic_pointer_cast<Variable>(expr)) {
        llvm::Type* ty = nullptr;
        llvm::Value* alloc = lookup_var(var->name, ty);
        if (alloc) return builder->CreateLoad(ty, alloc, var->name);
        error("Variable not found: " + var->name);
    }
    if (auto al = std::dynamic_pointer_cast<ArrayLit>(expr)) return build_array_constant(*al);
    if (auto ix = std::dynamic_pointer_cast<Index>(expr)) {
        llvm::Type* elem_ty = nullptr;
        llvm::Value* ptr = gen_index_ptr(*ix, elem_ty);
        return builder->CreateLoad(elem_ty, ptr, "idxtmp");
    }
    if (auto call = std::dynamic_pointer_cast<Call>(expr)) {
        auto it = functions.find(call->callee);
        if (it == functions.end()) error("Unknown function: " + call->callee);
        llvm::Function* func = it->second;
        std::vector<llvm::Value*> args;
        for (std::size_t i = 0; i < call->args.size(); ++i) {
            auto v = generate_expr(call->args[i]);
            if (!v) return nullptr;
            if (i < func->arg_size()) {
                llvm::Type* param_ty = func->getArg(i)->getType();
                if (param_ty != v->getType()) {
                    if (param_ty->isDoubleTy() && v->getType()->isIntegerTy(32)) {
                        v = builder->CreateSIToFP(v, builder->getDoubleTy(), "cast");
                    } else {
                        error("Argument " + std::to_string(i + 1) + " of '" + call->callee + "' expects " + llvm_type_name(param_ty) + ", got " + llvm_type_name(v->getType()));
                    }
                }
            }
            args.push_back(v);
        }
        if (func->getReturnType()->isVoidTy()) {
            builder->CreateCall(func, args);
            return nullptr;
        }
        return builder->CreateCall(func, args, "calltmp");
    }
    if (auto add = std::dynamic_pointer_cast<Add>(expr)) {
        auto left = generate_expr(add->left);
        auto right = generate_expr(add->right);
        if (!left || !right) return nullptr;
        require_numeric(left, "+");
        require_numeric(right, "+");
        if (left->getType()->isDoubleTy() || right->getType()->isDoubleTy()) {
            if (left->getType()->isIntegerTy(32)) left = builder->CreateSIToFP(left, builder->getDoubleTy());
            if (right->getType()->isIntegerTy(32)) right = builder->CreateSIToFP(right, builder->getDoubleTy());
            return builder->CreateFAdd(left, right, "addtmp");
        }
        return builder->CreateAdd(left, right, "addtmp");
    }
    if (auto sub = std::dynamic_pointer_cast<Sub>(expr)) {
        auto left = generate_expr(sub->left);
        auto right = generate_expr(sub->right);
        if (!left || !right) return nullptr;
        require_numeric(left, "-");
        require_numeric(right, "-");
        if (left->getType()->isDoubleTy() || right->getType()->isDoubleTy()) {
            if (left->getType()->isIntegerTy(32)) left = builder->CreateSIToFP(left, builder->getDoubleTy());
            if (right->getType()->isIntegerTy(32)) right = builder->CreateSIToFP(right, builder->getDoubleTy());
            return builder->CreateFSub(left, right, "subtmp");
        }
        return builder->CreateSub(left, right, "subtmp");
    }
    if (auto mul = std::dynamic_pointer_cast<Mul>(expr)) {
        auto left = generate_expr(mul->left);
        auto right = generate_expr(mul->right);
        if (!left || !right) return nullptr;
        require_numeric(left, "*");
        require_numeric(right, "*");
        if (left->getType()->isDoubleTy() || right->getType()->isDoubleTy()) {
            if (left->getType()->isIntegerTy(32)) left = builder->CreateSIToFP(left, builder->getDoubleTy());
            if (right->getType()->isIntegerTy(32)) right = builder->CreateSIToFP(right, builder->getDoubleTy());
            return builder->CreateFMul(left, right, "multmp");
        }
        return builder->CreateMul(left, right, "multmp");
    }
    if (auto div = std::dynamic_pointer_cast<Div>(expr)) {
        auto left = generate_expr(div->left);
        auto right = generate_expr(div->right);
        if (!left || !right) return nullptr;
        require_numeric(left, "/");
        require_numeric(right, "/");
        if (left->getType()->isDoubleTy() || right->getType()->isDoubleTy()) {
            if (left->getType()->isIntegerTy(32)) left = builder->CreateSIToFP(left, builder->getDoubleTy());
            if (right->getType()->isIntegerTy(32)) right = builder->CreateSIToFP(right, builder->getDoubleTy());
            return builder->CreateFDiv(left, right, "divtmp");
        }
        return builder->CreateSDiv(left, right, "divtmp");
    }
    if (auto neg = std::dynamic_pointer_cast<Neg>(expr)) {
        auto val = generate_expr(neg->value);
        if (!val) return nullptr;
        require_numeric(val, "unary -");
        if (val->getType()->isDoubleTy()) return builder->CreateFNeg(val, "negtmp");
        return builder->CreateNeg(val, "negtmp");
    }
    if (auto cmp = std::dynamic_pointer_cast<CmpOp>(expr)) {
        auto left = generate_expr(cmp->left);
        auto right = generate_expr(cmp->right);
        if (!left || !right) return nullptr;
        bool both_numeric = is_numeric(left->getType()) && is_numeric(right->getType());
        bool both_bool = left->getType()->isIntegerTy(1) && right->getType()->isIntegerTy(1);
        if (!both_numeric && !both_bool) error("Comparison operands must be both numeric or both bool");
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
    if (auto not_ = std::dynamic_pointer_cast<Not>(expr)) {
        auto val = generate_expr(not_->value);
        if (!val) return nullptr;
        require_bool(val, "!");
        return builder->CreateXor(val, builder->getInt1(true), "nottmp");
    }
    if (auto and_ = std::dynamic_pointer_cast<And>(expr)) return generate_logical_and(*and_);
    if (auto or_ = std::dynamic_pointer_cast<Or>(expr)) return generate_logical_or(*or_);

    return nullptr;
}

llvm::Value* CodeGenerator::generate_logical_and(const And& and_) {
    auto lhs = generate_expr(and_.left);
    if (!lhs) return nullptr;
    require_bool(lhs, "&&");

    auto current = builder->GetInsertBlock();
    llvm::Function* func = current->getParent();
    auto rhs_block = llvm::BasicBlock::Create(*context, "and.rhs", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "and.merge");

    builder->CreateCondBr(lhs, rhs_block, merge_block);
    builder->SetInsertPoint(rhs_block);
    auto rhs = generate_expr(and_.right);
    if (!rhs) return nullptr;
    require_bool(rhs, "&&");
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(merge_block);

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
    require_bool(lhs, "||");

    auto current = builder->GetInsertBlock();
    llvm::Function* func = current->getParent();
    auto rhs_block = llvm::BasicBlock::Create(*context, "or.rhs", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "or.merge");

    builder->CreateCondBr(lhs, merge_block, rhs_block);
    builder->SetInsertPoint(rhs_block);
    auto rhs = generate_expr(or_.right);
    if (!rhs) return nullptr;
    require_bool(rhs, "||");
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(merge_block);

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
    llvm::PHINode* phi = builder->CreatePHI(builder->getInt1Ty(), 2, "ortmp");
    phi->addIncoming(builder->getTrue(), current);
    phi->addIncoming(rhs, rhs_block);
    return phi;
}

llvm::Value* CodeGenerator::generate_icmp(const std::string& op, llvm::Value* left, llvm::Value* right) {
    if (op == "==") return builder->CreateICmpEQ(left, right, "cmptmp");
    if (op == "!=") return builder->CreateICmpNE(left, right, "cmptmp");
    if (op == "<")  return builder->CreateICmpSLT(left, right, "cmptmp");
    if (op == ">")  return builder->CreateICmpSGT(left, right, "cmptmp");
    if (op == "<=") return builder->CreateICmpSLE(left, right, "cmptmp");
    if (op == ">=") return builder->CreateICmpSGE(left, right, "cmptmp");
    error("Unknown icmp operator: " + op);
}

llvm::Value* CodeGenerator::generate_fcmp(const std::string& op, llvm::Value* left, llvm::Value* right) {
    if (op == "==") return builder->CreateFCmpOEQ(left, right, "cmptmp");
    if (op == "!=") return builder->CreateFCmpONE(left, right, "cmptmp");
    if (op == "<")  return builder->CreateFCmpOLT(left, right, "cmptmp");
    if (op == ">")  return builder->CreateFCmpOGT(left, right, "cmptmp");
    if (op == "<=") return builder->CreateFCmpOLE(left, right, "cmptmp");
    if (op == ">=") return builder->CreateFCmpOGE(left, right, "cmptmp");
    error("Unknown fcmp operator: " + op);
}

// ============================================================================
// Statements: Declarations, IO, Control Flow, Assignments
// ============================================================================

void CodeGenerator::generate_let(const Let& let) {
    llvm::Value* val = generate_expr(let.value);
    if (!val) return;
    llvm::Type* ty = val->getType();

    if (!let.var_type.empty()) {
        std::string base;
        int arr_size = -1;
        if (!parse_type(let.var_type, base, arr_size)) error("Unknown type: " + let.var_type);
        if (arr_size >= 0) {
            llvm::ArrayType* at = llvm::dyn_cast<llvm::ArrayType>(ty);
            if (!at) error("Type mismatch for '" + let.name + "': expected array of " + base);
            llvm::Type* scalar = scalar_type_for(base);
            if (at->getElementType() != scalar) error("Type mismatch for '" + let.name + "': unexpected element type");
            if (arr_size > 0 && (std::size_t)arr_size != at->getNumElements()) error("Array size mismatch");
        } else {
            llvm::Type* declared = scalar_type_for(base);
            if (declared->isDoubleTy() && val->getType()->isIntegerTy(32)) {
                val = builder->CreateSIToFP(val, builder->getDoubleTy(), "cast");
                ty = declared;
            } else if (declared != val->getType()) {
                error("Type mismatch for '" + let.name + "': cannot assign " + llvm_type_name(val->getType()) + " to " + let.var_type);
            }
        }
    }

    auto alloca = builder->CreateAlloca(ty, nullptr, let.name);
    builder->CreateStore(val, alloca);
    define_var(let.name, alloca, ty);
}

void CodeGenerator::generate_assign(const Assign& a) {
    if (auto ix = std::dynamic_pointer_cast<Index>(a.name)) {
        llvm::Type* elem_ty = nullptr;
        llvm::Value* ptr = gen_index_ptr(*ix, elem_ty);
        llvm::Value* val = generate_expr(a.value);
        if (!val) return;
        if (elem_ty->isDoubleTy() && val->getType()->isIntegerTy(32)) {
            val = builder->CreateSIToFP(val, builder->getDoubleTy(), "cast");
        } else if (elem_ty->isArrayTy()) {
            error("Cannot assign an array to a single array element");
        } else if (elem_ty != val->getType()) {
            error("Type mismatch for indexed assignment");
        }
        builder->CreateStore(val, ptr);
        return;
    }

    auto var = std::dynamic_pointer_cast<Variable>(a.name);
    if (!var) error("Invalid assignment target");

    llvm::Type* ty = nullptr;
    llvm::Value* alloc = lookup_var(var->name, ty);
    if (!alloc) error("Cannot assign to unknown variable: " + var->name);
    llvm::Value* val = generate_expr(a.value);
    if (!val) return;

    llvm::Type* val_ty = val->getType();
    if (ty->isDoubleTy() && val_ty->isIntegerTy(32)) {
        val = builder->CreateSIToFP(val, builder->getDoubleTy(), "cast");
    } else if (ty != val_ty) {
        error("Type mismatch for '" + var->name + "': cannot assign " + llvm_type_name(val_ty) + " to " + llvm_type_name(ty));
    }

    builder->CreateStore(val, alloc);
}

void CodeGenerator::generate_hot(const Hot& hot) {
    for (const auto& stmt : hot.body) generate_stmt(stmt);
}

void CodeGenerator::generate_print(const Print& print) {
    auto val = generate_expr(print.value);
    if (!val) return;

    llvm::Value* format_str = nullptr;
    llvm::Value* arg = val;

    if (val->getType()->isIntegerTy(32)) format_str = builder->CreateGlobalString("%d\n", "format");
    else if (val->getType()->isDoubleTy()) format_str = builder->CreateGlobalString("%f\n", "format");
    else if (val->getType()->isIntegerTy(1)) {
        format_str = builder->CreateGlobalString("%d\n", "format");
        arg = builder->CreateZExt(val, builder->getInt32Ty());
    }
    else if (val->getType()->isPointerTy()) format_str = builder->CreateGlobalString("%s\n", "format");
    else error("Cannot print a value of type " + llvm_type_name(val->getType()));

    auto printf_type = llvm::FunctionType::get(builder->getInt32Ty(), llvm::PointerType::getUnqual(*context), true);
    auto printf_func = module->getOrInsertFunction("printf", printf_type);
    builder->CreateCall(printf_func, {format_str, arg});
}

void CodeGenerator::generate_block(const std::shared_ptr<Block>& block) {
    if (!block) return;
    push_scope();
    for (const auto& stmt : block->body) generate_stmt(stmt);
    pop_scope();
}

void CodeGenerator::generate_if(const If& if_stmt) {
    auto cond = generate_expr(if_stmt.condition);
    if (!cond) return;
    require_bool(cond, "if");

    auto current = builder->GetInsertBlock();
    if (current->getTerminatorOrNull()) return;

    llvm::Function* func = current->getParent();
    auto then_block = llvm::BasicBlock::Create(*context, "then", func);
    auto else_block = llvm::BasicBlock::Create(*context, "else", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "merge");

    builder->CreateCondBr(cond, then_block, else_block);
    builder->SetInsertPoint(then_block);
    generate_block(if_stmt.then_block);
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(merge_block);

    builder->SetInsertPoint(else_block);
    generate_block(if_stmt.else_block);
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(merge_block);

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
}

void CodeGenerator::generate_while(const While& w) {
    auto func = builder->GetInsertBlock()->getParent();
    auto cond_block = llvm::BasicBlock::Create(*context, "while.cond", func);
    auto body_block = llvm::BasicBlock::Create(*context, "while.body", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "while.end");

    builder->CreateBr(cond_block);
    builder->SetInsertPoint(cond_block);
    auto cond = generate_expr(w.condition);
    if (!cond) return;
    require_bool(cond, "while");
    builder->CreateCondBr(cond, body_block, merge_block);

    builder->SetInsertPoint(body_block);
    generate_block(w.body);
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(cond_block);

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
}

void CodeGenerator::generate_for(const For& f) {
    auto func = builder->GetInsertBlock()->getParent();
    if (f.init) generate_stmt(f.init);

    auto cond_block = llvm::BasicBlock::Create(*context, "for.cond", func);
    auto body_block = llvm::BasicBlock::Create(*context, "for.body", func);
    auto step_block = llvm::BasicBlock::Create(*context, "for.step", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "for.end");

    builder->CreateBr(cond_block);
    builder->SetInsertPoint(cond_block);
    if (f.condition) {
        auto cond = generate_expr(f.condition);
        if (!cond) return;
        require_bool(cond, "for condition");
        builder->CreateCondBr(cond, body_block, merge_block);
    } else {
        builder->CreateBr(body_block);
    }

    builder->SetInsertPoint(body_block);
    generate_block(f.body);
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(step_block);

    builder->SetInsertPoint(step_block);
    if (f.step) generate_stmt(f.step);
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(cond_block);

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
}

void CodeGenerator::generate_for_in(const ForIn& fi) {
    auto func = builder->GetInsertBlock()->getParent();
    llvm::Value* arr = generate_expr(fi.iterable);
    if (!arr) return;
    auto at = llvm::dyn_cast<llvm::ArrayType>(arr->getType());
    if (!at) error("for-in requires an array, got " + llvm_type_name(arr->getType()));
    auto N = (int)at->getNumElements();
    llvm::Type* elem = at->getElementType();

    auto tmp = builder->CreateAlloca(at, nullptr, "forin.arr");
    builder->CreateStore(arr, tmp);

    push_scope();
    auto xalloc = builder->CreateAlloca(elem, nullptr, fi.var);
    define_var(fi.var, xalloc, elem);

    auto i = builder->CreateAlloca(builder->getInt32Ty(), nullptr, "forin.i");
    builder->CreateStore(builder->getInt32(0), i);

    auto cond_block = llvm::BasicBlock::Create(*context, "forin.cond", func);
    auto body_block = llvm::BasicBlock::Create(*context, "forin.body", func);
    auto step_block = llvm::BasicBlock::Create(*context, "forin.step", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "forin.end");

    builder->CreateBr(cond_block);
    builder->SetInsertPoint(cond_block);
    auto icur = builder->CreateLoad(builder->getInt32Ty(), i, "forin.i.cur");
    auto cmp = builder->CreateICmpSLT(icur, builder->getInt32(N), "forin.cmp");
    builder->CreateCondBr(cmp, body_block, merge_block);

    builder->SetInsertPoint(body_block);
    auto eptr = builder->CreateInBoundsGEP(at, tmp, {builder->getInt32(0), icur}, "forin.elem");
    auto ev = builder->CreateLoad(elem, eptr, "forin.val");
    builder->CreateStore(ev, xalloc);
    generate_block(fi.body);
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(step_block);

    builder->SetInsertPoint(step_block);
    auto inext = builder->CreateAdd(icur, builder->getInt32(1), "forin.i.next");
    builder->CreateStore(inext, i);
    builder->CreateBr(cond_block);

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
    pop_scope();
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
            else if (ret_ty->isArrayTy()) builder->CreateRet(llvm::ConstantAggregateZero::get(ret_ty));
            else builder->CreateRet(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ret_ty)));
        } else {
            builder->CreateRetVoid();
        }
    }
}

// ============================================================================
// Function Declaration & Body Generation
// ============================================================================

void CodeGenerator::declare_func(const FuncDecl& fd) {
    std::vector<llvm::Type*> param_types;
    for (auto& p : fd.params) {
        llvm::Type* t = signature_type_for(p.var_type, "parameter '" + p.name + "'");
        if (!t) t = builder->getInt32Ty();
        param_types.push_back(t);
    }
    llvm::Type* ret_ty = signature_type_for(fd.return_type, "return type of '" + fd.name + "'");
    if (!ret_ty) ret_ty = builder->getVoidTy();

    auto func_type = llvm::FunctionType::get(ret_ty, param_types, false);
    auto func = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage, fd.name, module.get());
    functions[fd.name] = func;
}

void CodeGenerator::generate_func_decl(const FuncDecl& fd) {
    auto it = functions.find(fd.name);
    if (it == functions.end()) return;
    llvm::Function* func = it->second;

    auto entry = llvm::BasicBlock::Create(*context, "entry", func);
    builder->SetInsertPoint(entry);

    push_scope();
    return_type_stack.push_back(func->getReturnType()->isVoidTy() ? nullptr : func->getReturnType());

    auto arg_it = func->arg_begin();
    for (auto& p : fd.params) {
        llvm::Argument& arg = *arg_it++;
        llvm::Type* pt = signature_type_for(p.var_type, "parameter '" + p.name + "'");
        if (!pt) pt = builder->getInt32Ty();
        auto alloca = builder->CreateAlloca(pt, nullptr, p.name);
        builder->CreateStore(&arg, alloca);
        define_var(p.name, alloca, pt);
    }

    generate_block(fd.body);
    if (!block_has_terminator(builder->GetInsertBlock())) {
        llvm::Type* ret_ty = func->getReturnType();
        if (ret_ty->isVoidTy()) builder->CreateRetVoid();
        else if (ret_ty->isIntegerTy(32)) builder->CreateRet(builder->getInt32(0));
        else if (ret_ty->isDoubleTy()) builder->CreateRet(llvm::ConstantFP::get(builder->getDoubleTy(), 0.0));
        else if (ret_ty->isIntegerTy(1)) builder->CreateRet(builder->getInt1(false));
        else if (ret_ty->isArrayTy()) builder->CreateRet(llvm::ConstantAggregateZero::get(ret_ty));
        else builder->CreateRet(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ret_ty)));
    }

    return_type_stack.pop_back();
    pop_scope();
}

void CodeGenerator::generate_stmt(const std::shared_ptr<Stmt>& stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case Stmt::Kind::Let:      generate_let(stmt->let); break;
        case Stmt::Kind::Hot:      generate_hot(stmt->hot); break;
        case Stmt::Kind::Print:    generate_print(stmt->print); break;
        case Stmt::Kind::If:       if (stmt->if_stmt) generate_if(*stmt->if_stmt); break;
        case Stmt::Kind::While:    if (stmt->while_stmt) generate_while(*stmt->while_stmt); break;
        case Stmt::Kind::For:      if (stmt->for_stmt) generate_for(*stmt->for_stmt); break;
        case Stmt::Kind::ForIn:    if (stmt->for_in_stmt) generate_for_in(*stmt->for_in_stmt); break;
        case Stmt::Kind::Return:   if (stmt->return_stmt) generate_return(*stmt->return_stmt); break;
        case Stmt::Kind::Assign:   if (stmt->assign_stmt) generate_assign(*stmt->assign_stmt); break;
        case Stmt::Kind::FuncDecl: generate_func_decl(*stmt->func_decl); break;
        case Stmt::Kind::CallStmt: if (stmt->call_stmt) generate_expr(stmt->call_stmt); break;
    }
}

bool CodeGenerator::block_has_terminator(llvm::BasicBlock* bb) {
    if (bb->empty()) return false;
    return bb->back().isTerminator();
}

void CodeGenerator::finalize() {
    auto cur = builder->GetInsertBlock();
    if (cur && !cur->getTerminatorOrNull()) {
        builder->CreateRet(builder->getInt32(0));
    }
    llvm::verifyFunction(*main_func, &llvm::errs());
    module->print(llvm::outs(), nullptr);
}

void CodeGenerator::generate_program(const Program& program) {
    for (const auto& stmt : program.body) {
        if (stmt->kind == Stmt::Kind::FuncDecl && stmt->func_decl) declare_func(*stmt->func_decl);
    }
    for (const auto& stmt : program.body) {
        if (stmt->kind == Stmt::Kind::FuncDecl && stmt->func_decl) generate_func_decl(*stmt->func_decl);
    }
    builder->SetInsertPoint(main_entry_block);
    for (const auto& stmt : program.body) {
        if (stmt->kind != Stmt::Kind::FuncDecl) generate_stmt(stmt);
    }
}
