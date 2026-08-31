#pragma once
#include "json_reader.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <map>
#include <memory>

class CodeGenerator {
private:
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    llvm::Function* main_func;
    llvm::BasicBlock* entry_block;
    std::map<std::string, llvm::Value*> named_values;
    std::map<std::string, llvm::Type*> named_types;

public:
    CodeGenerator();

    void generate_program(const Program& program);
    llvm::Value* generate_expr(const std::shared_ptr<Expr>& expr);
    void generate_let(const Let& let);
    void generate_hot(const Hot& hot);
    void generate_print(const Print& print);
    void finalize();
};