#pragma once
#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct Expr {
    virtual ~Expr() = default;
    int line = 0;   // 1-based source line of the node, 0 if unknown
    int col = 0;    // 1-based source column of the node, 0 if unknown
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

struct ArrayLit : public Expr {
    std::vector<std::shared_ptr<Expr>> elements;
};

struct Index : public Expr {
    std::shared_ptr<Expr> object;
    std::shared_ptr<Expr> index;
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

struct MemberAccess : public Expr {
    std::shared_ptr<Expr> object;
    std::string member;
};

struct StructLiteral : public Expr {
    std::string name;
    std::vector<std::shared_ptr<Expr>> args;
};

struct New : public Expr {
    std::string base;
    std::shared_ptr<Expr> size;   // Number for new int[3]; any int expr for new int[n]
};

// Statements
struct Stmt;

struct Let {
    std::string name;
    std::shared_ptr<Expr> value;
    std::string var_type;   // "int" | "float" | "bool" | "string" | "" (auto/inferred)
};

struct Hot {
    std::vector<std::shared_ptr<Stmt>> body;
};

struct Epoch {
    std::vector<std::shared_ptr<Stmt>> body;
};

struct Print {
    std::vector<std::shared_ptr<Expr>> args;
};

struct Block {
    std::vector<std::shared_ptr<Stmt>> body;
};

struct Param {
    std::string name;
    std::string var_type;
};

struct If {
    std::shared_ptr<Expr> condition;
    std::shared_ptr<Block> then_block;
    std::shared_ptr<Block> else_block;
};

struct While {
    std::shared_ptr<Expr> condition;
    std::shared_ptr<Block> body;
};

struct For {
    std::shared_ptr<Stmt> init;
    std::shared_ptr<Expr> condition;
    std::shared_ptr<Stmt> step;
    std::shared_ptr<Block> body;
};

struct ForIn {
    std::string var;
    std::shared_ptr<Expr> iterable;
    std::shared_ptr<Block> body;
};

struct Return {
    std::shared_ptr<Expr> value;
};

struct Break {
};

struct Continue {
};

struct Assign {
    std::shared_ptr<Expr> name;   // Variable, Index or MemberAccess (lvalue)
    std::shared_ptr<Expr> value;
    std::string op = "=";         // "=", "+=", "-=", "*=", "/="
};

struct FuncDecl {
    std::string name;
    std::vector<Param> params;
    std::string return_type;
    std::shared_ptr<Block> body;
};

struct StructField {
    std::string name;
    std::string var_type;
};

struct StructDecl {
    std::string name;
    std::vector<StructField> fields;
};

struct Stmt {
    enum class Kind { Let, Hot, Epoch, Print, If, While, For, ForIn, Return, Assign, FuncDecl, CallStmt, StructDecl, Break, Continue };
    Kind kind;
    int line = 0;   // 1-based source line of the node, 0 if unknown
    int col = 0;    // 1-based source column of the node, 0 if unknown
    Let let;
    Hot hot;
    Epoch epoch;
    Print print;
    std::shared_ptr<If> if_stmt;
    std::shared_ptr<While> while_stmt;
    std::shared_ptr<For> for_stmt;
    std::shared_ptr<ForIn> for_in_stmt;
    std::shared_ptr<Return> return_stmt;
    std::shared_ptr<Assign> assign_stmt;
    std::shared_ptr<FuncDecl> func_decl;
    std::shared_ptr<Call> call_stmt;
    std::shared_ptr<StructDecl> struct_decl;
    Break break_stmt;
    Continue continue_stmt;
};

struct Program {
    std::vector<std::shared_ptr<Stmt>> body;
};


Program parse_json(const std::string& filename);