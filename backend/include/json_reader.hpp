#pragma once
#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct Expr {
    virtual ~Expr() = default;
};

// Expressions
struct Number : public Expr {
    int value;
};

struct Float : public Expr {
    double value;
};

struct Bool : public Expr {
    bool value;
};

struct String : public Expr {
    std::string value;
};

struct Variable : public Expr {
    std::string name;
};

struct Add : public Expr {
    std::shared_ptr<Expr> left;
    std::shared_ptr<Expr> right;
};

struct Sub : public Expr {
    std::shared_ptr<Expr> left;
    std::shared_ptr<Expr> right;
};

struct Mul : public Expr {
    std::shared_ptr<Expr> left;
    std::shared_ptr<Expr> right;
};

struct Div : public Expr {
    std::shared_ptr<Expr> left;
    std::shared_ptr<Expr> right;
};

struct Neg : public Expr {
    std::shared_ptr<Expr> value;
};

struct CmpOp : public Expr {
    std::string op;
    std::shared_ptr<Expr> left;
    std::shared_ptr<Expr> right;
};

struct And : public Expr {
    std::shared_ptr<Expr> left;
    std::shared_ptr<Expr> right;
};

struct Or : public Expr {
    std::shared_ptr<Expr> left;
    std::shared_ptr<Expr> right;
};

struct Not : public Expr {
    std::shared_ptr<Expr> value;
};

struct Call : public Expr {
    std::string callee;
    std::vector<std::shared_ptr<Expr>> args;
};

// Statements
struct Let {
    std::string name;
    std::shared_ptr<Expr> value;
    std::string var_type;   // "int" | "float" | "bool" | "string" | "" (auto/inferred)
};

struct Hot {
    std::vector<Let> body;
};

struct Print {
    std::shared_ptr<Expr> value;
};

struct Stmt;
struct Block {
    std::vector<std::shared_ptr<Stmt>> body;
};

struct Param {
    std::string name;
    std::string var_type;
};

struct If : public Expr {
    std::shared_ptr<Expr> condition;
    std::shared_ptr<Block> then_block;
    std::shared_ptr<Block> else_block;
};

struct While : public Expr {
    std::shared_ptr<Expr> condition;
    std::shared_ptr<Block> body;
};

struct For {
    std::shared_ptr<Stmt> init;
    std::shared_ptr<Expr> condition;
    std::shared_ptr<Stmt> step;
    std::shared_ptr<Block> body;
};

struct Return {
    std::shared_ptr<Expr> value;
};

struct Assign {
    std::string name;
    std::shared_ptr<Expr> value;
};

struct FuncDecl {
    std::string name;
    std::vector<Param> params;
    std::string return_type;
    std::shared_ptr<Block> body;
};

struct Stmt {
    enum class Kind { Let, Hot, Print, If, While, For, Return, Assign, FuncDecl, CallStmt };
    Kind kind;
    Let let;
    Hot hot;
    Print print;
    std::shared_ptr<If> if_stmt;
    std::shared_ptr<While> while_stmt;
    std::shared_ptr<For> for_stmt;
    std::shared_ptr<Return> return_stmt;
    std::shared_ptr<Assign> assign_stmt;
    std::shared_ptr<FuncDecl> func_decl;
    std::shared_ptr<Call> call_stmt;
};

struct Program {
    std::vector<std::shared_ptr<Stmt>> body;
};


Program parse_json(const std::string& filename);