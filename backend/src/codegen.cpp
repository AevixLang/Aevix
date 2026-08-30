#include "codegen.hpp"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include <iostream>
#include <map>

CodeGenerator::CodeGenerator()
    : context(std::make_unique<llvm::LLVMContext>())
    , module(std::make_unique<llvm::Module>("velo", *context))
    , builder(std::make_unique<llvm::IRBuilder<>>(*context))
{
    auto func_type = llvm::FunctionType::get(builder->getInt32Ty(), {}, false);
    main_func = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage, "main", module.get());
    entry_block = llvm::BasicBlock::Create(*context, "entry", main_func);
    builder->SetInsertPoint(entry_block);
}

llvm::Value* CodeGenerator::generate_expr(const std::shared_ptr<Expr>& expr) {
    if (!expr) return nullptr;

    if (auto num = std::dynamic_pointer_cast<Number>(expr)) {
        return builder->getInt32(num->value);
    }
    else if (auto var = std::dynamic_pointer_cast<Variable>(expr)) {
        if (named_values.find(var->name) != named_values.end()) {
            return builder->CreateLoad(builder->getInt32Ty(), named_values[var->name], var->name);
        }
        std::cerr << "❌ Variable not found: " << var->name << std::endl;
        return nullptr;
    }
    else if (auto add = std::dynamic_pointer_cast<Add>(expr)) {
        auto left = generate_expr(add->left);
        auto right = generate_expr(add->right);
        return builder->CreateAdd(left, right, "addtmp");
    }
    else if (auto sub = std::dynamic_pointer_cast<Sub>(expr)) {
        auto left = generate_expr(sub->left);
        auto right = generate_expr(sub->right);
        return builder->CreateSub(left, right, "subtmp");
    }
    else if (auto mul = std::dynamic_pointer_cast<Mul>(expr)) {
        auto left = generate_expr(mul->left);
        auto right = generate_expr(mul->right);
        return builder->CreateMul(left, right, "multmp");
    }
    else if (auto div = std::dynamic_pointer_cast<Div>(expr)) {
        auto left = generate_expr(div->left);
        auto right = generate_expr(div->right);
        return builder->CreateSDiv(left, right, "divtmp");
    }
    else if (auto neg = std::dynamic_pointer_cast<Neg>(expr)) {
        auto val = generate_expr(neg->value);
        return builder->CreateNeg(val, "negtmp");
    }
    
    return nullptr;
}

void CodeGenerator::generate_let(const Let& let) {
    llvm::Value* val = generate_expr(let.value);
    if (val) {
        auto alloca = builder->CreateAlloca(builder->getInt32Ty(), nullptr, let.name);
        builder->CreateStore(val, alloca);
        named_values[let.name] = alloca;
    }
}

void CodeGenerator::generate_hot(const Hot& hot) {
    for (const auto& let : hot.body) {
        generate_let(let);
    }
}

void CodeGenerator::generate_print(const Print& print) {
    auto val = generate_expr(print.value);
    if (val) {
        auto printf_type = llvm::FunctionType::get(
            builder->getInt32Ty(), 
            llvm::PointerType::get(builder->getInt8Ty(), 0),
            true
        );
        auto printf_func = llvm::Function::Create(
            printf_type, 
            llvm::Function::ExternalLinkage, 
            "printf", 
            module.get()
        );
        
        auto format_str = builder->CreateGlobalString("%d\n", "format");
        
        builder->CreateCall(printf_func, {format_str, val});
    }
}

void CodeGenerator::finalize() {
    builder->CreateRet(builder->getInt32(0));
    
    llvm::verifyFunction(*main_func, &llvm::errs());
    
    module->print(llvm::outs(), nullptr);
}

void CodeGenerator::generate_program(const Program& program) {
    for (const auto& let : program.lets) {
        generate_let(let);
    }
    
    for (const auto& hot : program.hots) {
        generate_hot(hot);
    }
    
    for (const auto& print : program.prints) {
        generate_print(print);
    }
}