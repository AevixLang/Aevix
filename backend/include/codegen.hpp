#pragma once
#include "json_reader.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

class CodeGenerator {
private:
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    llvm::Function* main_func;
    llvm::BasicBlock* main_entry_block;
    std::map<std::string, llvm::Function*> functions;
    std::vector<std::map<std::string, llvm::Value*>> named_values;
    std::vector<std::map<std::string, llvm::Type*>> named_types;
    std::vector<llvm::Type*> return_type_stack;

    void push_scope();
    void pop_scope();
    void define_var(const std::string& name, llvm::Value* alloc, llvm::Type* ty);
    llvm::Value* lookup_var(const std::string& name, llvm::Type*& ty);
    llvm::Type* llvm_type_for(const std::string& tn);
    [[noreturn]] void error(const std::string& msg);

public:
    CodeGenerator();

    void generate_program(const Program& program);
    llvm::Value* generate_expr(const std::shared_ptr<Expr>& expr);
    llvm::Value* generate_icmp(const std::string& op, llvm::Value* left, llvm::Value* right);
    llvm::Value* generate_fcmp(const std::string& op, llvm::Value* left, llvm::Value* right);
    void generate_stmt(const std::shared_ptr<Stmt>& stmt);
    void generate_let(const Let& let);
    void generate_hot(const Hot& hot);
    void generate_print(const Print& print);
    void generate_block(const std::shared_ptr<Block>& block);
    void generate_if(const If& if_stmt);
    void generate_while(const While& w);
    void generate_for(const For& f);
    void generate_return(const Return& r);
    void generate_assign(const Assign& a);
    void declare_func(const FuncDecl& fd);
    void generate_func_decl(const FuncDecl& fd);
    llvm::Value* generate_logical_and(const And& and_);
    llvm::Value* generate_logical_or(const Or& or_);
    bool block_has_terminator(llvm::BasicBlock* bb);
    void finalize();
};