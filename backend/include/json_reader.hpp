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

// Operators
struct Let {
    std::string name;
    std::shared_ptr<Expr> value;
    std::string var_type;   // "int" | "float" | "bool" | "string" | "" (auto)
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

struct If : public Expr {
    std::shared_ptr<Expr> condition;
    std::shared_ptr<Block> then_block;
    std::shared_ptr<Block> else_block;   // null if no else
};

struct Stmt {
    enum class Kind { Let, Hot, Print, If };
    Kind kind;
    Let let;
    Hot hot;
    Print print;
    std::shared_ptr<If> if_stmt;
};

struct Program {
    std::vector<std::shared_ptr<Stmt>> body;
};


Program parse_json(const std::string& filename);