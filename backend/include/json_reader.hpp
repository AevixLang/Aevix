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