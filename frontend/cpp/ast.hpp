#pragma once
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <memory>
#include <cstdint>

namespace aevix {

// Forward declarations
struct Expr;
struct Stmt;
struct Param;
struct StructField;

using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;

// ============================================================================
// Types
// ============================================================================

struct TypeName {
    std::string name; // "int", "float", "bool", "string", "i8", etc.
    std::optional<int> array_size; // None for open arrays, Some(n) for fixed

    bool is_array() const { return array_size.has_value() || (name.size() > 2 && name.back() == ']'); }
};

// ============================================================================
// Expressions
// ============================================================================

struct Number   { int64_t value; };
struct FloatLit { double value; };
struct BoolLit  { bool value; };
struct StringLit { std::string value; };
struct Variable { std::string name; };

struct ArrayLit {
    std::vector<ExprPtr> elements;
};

struct NewExpr {
    std::string base_type; // "int", "float", "Point", etc.
    ExprPtr size;
};

struct Call {
    std::string callee;
    std::vector<ExprPtr> args;
};

struct MemberAccess {
    ExprPtr object;
    std::string member;
};

struct Index {
    ExprPtr object;
    ExprPtr index;
};

struct StructLiteral {
    std::string name;
    std::vector<ExprPtr> args;
    // Parallel to args: the field each argument initializes, or "" when the
    // argument is positional. Named and positional fields cannot be mixed.
    std::vector<std::string> field_names;
};

struct UnaryMinus { ExprPtr operand; };
struct LogicalNot { ExprPtr operand; };

struct Add { ExprPtr left, right; };
struct Sub { ExprPtr left, right; };
struct Mul { ExprPtr left, right; };
struct Div { ExprPtr left, right; };
struct Mod { ExprPtr left, right; };

struct CmpOp {
    std::string op; // ==, !=, <, >, <=, >=
    ExprPtr left, right;
};

struct And { ExprPtr left, right; };
struct Or  { ExprPtr left, right; };

using ExprVariant = std::variant<
    Number, FloatLit, BoolLit, StringLit, Variable,
    ArrayLit, NewExpr, Call, MemberAccess, Index, StructLiteral,
    UnaryMinus, LogicalNot,
    Add, Sub, Mul, Div, Mod,
    CmpOp, And, Or
>;

struct Expr {
    ExprVariant node;
    int line = 0;
    int col = 0;
};

// ============================================================================
// Statements
// ============================================================================

struct Import { std::string spec; std::string alias; }; // alias empty = flat merge

struct Let {
    std::string name;
    ExprPtr value;
    std::optional<std::string> var_type;
};

struct Hot  { std::vector<StmtPtr> body; };
struct Epoch { std::vector<StmtPtr> body; };

struct Print { std::vector<ExprPtr> args; };
struct Break {};
struct Continue {};

struct If {
    ExprPtr condition;
    std::vector<StmtPtr> then_body;
    std::optional<std::vector<StmtPtr>> else_body; // can be another If or block
};

struct While {
    ExprPtr condition;
    std::vector<StmtPtr> body;
};

struct For {
    std::optional<StmtPtr> init;
    std::optional<ExprPtr> condition;
    std::optional<StmtPtr> step;
    std::vector<StmtPtr> body;
};

struct ForIn {
    std::string var;
    ExprPtr iterable;
    std::vector<StmtPtr> body;
};

struct Return {
    std::optional<ExprPtr> value;
};

struct MatchCase {
    ExprPtr value;
    std::vector<StmtPtr> body;
};

struct Match {
    ExprPtr scrutinee;
    std::vector<MatchCase> cases;
    std::optional<std::vector<StmtPtr>> default_body;
};

struct Assign {
    ExprPtr target;
    std::string op; // =, +=, -=, *=, /=
    ExprPtr value;
};

struct Param {
    std::string name;
    std::optional<std::string> var_type;
    bool is_ref = false;
};

struct FuncDecl {
    std::string name;
    std::vector<Param> params;
    std::optional<std::string> return_type;
    std::vector<StmtPtr> body;
};

struct StructDecl {
    std::string name;
    std::vector<StructField> fields;
};

struct StructField {
    std::string name;
    std::string var_type;
};

struct EnumDecl {
    std::string name;
    std::vector<std::string> variants;
};

using StmtVariant = std::variant<
    Import, Let, Hot, Epoch, Print, Break, Continue,
    If, While, For, ForIn, Return, Assign, Match,
    FuncDecl, StructDecl, EnumDecl, Expr
>;

struct Stmt {
    StmtVariant node;
    int line = 0;
    int col = 0;
};

struct Program {
    std::vector<StmtPtr> body;
};

} // namespace aevix
