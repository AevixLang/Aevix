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

struct Program {
    std::vector<Let> lets;
    std::vector<Hot> hots;
    std::vector<Print> prints;
};


Program parse_json(const std::string& filename);