#pragma once
#include "ast.hpp"
#include <sstream>
#include <iostream>

namespace aevix {

class JsonWriter {
public:
    static std::string to_json(const Program& prog) {
        std::ostringstream os;
        os << "{\n  \"schemaVersion\": " << SCHEMA_VERSION << ",\n  \"type\": \"Program\",\n  \"body\": ";
        write_stmts(os, prog.body, 2);
        os << "\n}\n";
        return os.str();
    }

private:
    static constexpr int SCHEMA_VERSION = 4;

    static void indent(std::ostringstream& os, int depth) {
        os << std::string(depth, ' ');
    }

    static void write_stmts(std::ostringstream& os, const std::vector<StmtPtr>& stmts, int depth) {
        os << "[\n";
        for (size_t i = 0; i < stmts.size(); i++) {
            indent(os, depth + 2);
            write_stmt(os, *stmts[i], depth + 2);
            if (i + 1 < stmts.size()) os << ",";
            os << "\n";
        }
        indent(os, depth);
        os << "]";
    }

    static void write_exprs(std::ostringstream& os, const std::vector<ExprPtr>& exprs, int depth) {
        os << "[";
        for (size_t i = 0; i < exprs.size(); i++) {
            if (i > 0) os << ", ";
            write_expr(os, *exprs[i], depth);
        }
        os << "]";
    }

    static void write_opt_expr(std::ostringstream& os, const std::optional<ExprPtr>& e, int depth) {
        if (e) write_expr(os, **e, depth);
        else os << "null";
    }

    static void write_opt_stmts(std::ostringstream& os, const std::optional<std::vector<StmtPtr>>& s, int depth) {
        if (s) write_stmts(os, *s, depth);
        else os << "null";
    }

    static void write_stmt(std::ostringstream& os, const Stmt& s, int depth) {
        std::ostringstream node;
        std::visit([&](const auto& n) { write_stmt_node(node, n, depth); }, s.node);
        os << with_pos(node.str(), s.line, s.col);
    }

    static void write_expr(std::ostringstream& os, const Expr& e, int depth) {
        std::ostringstream node;
        std::visit([&](const auto& n) { write_expr_node(node, n, depth); }, e.node);
        os << with_pos(node.str(), e.line, e.col);
    }

    // Positions are part of schema v4 — the Python frontend already emits
    // them. Without them json_reader leaves line/col at 0 and every
    // downstream diagnostic loses its source location.
    static std::string with_pos(const std::string& obj, int line, int col) {
        size_t brace = obj.find('{');
        if (brace == std::string::npos) return obj;
        std::ostringstream os;
        os << obj.substr(0, brace + 1) << "\"line\": " << line << ", \"col\": " << col;
        if (brace + 1 < obj.size() && obj[brace + 1] != '}') os << ", ";
        os << obj.substr(brace + 1);
        return os.str();
    }

    // ---- Statement writers ----

    static void write_stmt_node(std::ostringstream& os, const Import& n, int d) {
        os << "{\"type\": \"Import\", \"spec\": \"" << escape(n.spec) << "\"}";
    }

    static void write_stmt_node(std::ostringstream& os, const Let& n, int d) {
        os << "{\"type\": \"Let\", \"name\": \"" << escape(n.name) << "\"";
        if (n.var_type) os << ", \"var_type\": \"" << escape(*n.var_type) << "\"";
        os << ", \"value\": "; write_expr(os, *n.value, d);
        os << "}";
    }

    static void write_stmt_node(std::ostringstream& os, const Hot& n, int d) {
        os << "{\"type\": \"Hot\", \"body\": "; write_stmts(os, n.body, d); os << "}";
    }

    static void write_stmt_node(std::ostringstream& os, const Epoch& n, int d) {
        os << "{\"type\": \"Epoch\", \"body\": "; write_stmts(os, n.body, d); os << "}";
    }

    static void write_stmt_node(std::ostringstream& os, const Print& n, int d) {
        os << "{\"type\": \"Print\", \"args\": "; write_exprs(os, n.args, d); os << "}";
    }

    static void write_stmt_node(std::ostringstream& os, const Break&, int) {
        os << "{\"type\": \"Break\"}";
    }

    static void write_stmt_node(std::ostringstream& os, const Continue&, int) {
        os << "{\"type\": \"Continue\"}";
    }

    static void write_stmt_node(std::ostringstream& os, const If& n, int d) {
        os << "{\"type\": \"If\", \"condition\": "; write_expr(os, *n.condition, d);
        os << ", \"then_body\": "; write_stmts(os, n.then_body, d);
        if (n.else_body) {
            os << ", \"else_body\": "; write_opt_stmts(os, n.else_body, d);
        }
        os << "}";
    }

    static void write_stmt_node(std::ostringstream& os, const While& n, int d) {
        os << "{\"type\": \"While\", \"condition\": "; write_expr(os, *n.condition, d);
        os << ", \"body\": "; write_stmts(os, n.body, d); os << "}";
    }

    static void write_stmt_node(std::ostringstream& os, const For& n, int d) {
        os << "{\"type\": \"For\"";
        if (n.init) { os << ", \"init\": "; write_stmt(os, **n.init, d); }
        if (n.condition) { os << ", \"condition\": "; write_expr(os, **n.condition, d); }
        if (n.step) { os << ", \"step\": "; write_stmt(os, **n.step, d); }
        os << ", \"body\": "; write_stmts(os, n.body, d); os << "}";
    }

    static void write_stmt_node(std::ostringstream& os, const ForIn& n, int d) {
        os << "{\"type\": \"ForIn\", \"var\": \"" << escape(n.var) << "\"";
        os << ", \"iterable\": "; write_expr(os, *n.iterable, d);
        os << ", \"body\": "; write_stmts(os, n.body, d); os << "}";
    }

    static void write_stmt_node(std::ostringstream& os, const Return& n, int d) {
        os << "{\"type\": \"Return\"";
        if (n.value) { os << ", \"value\": "; write_expr(os, **n.value, d); }
        os << "}";
    }

    static void write_stmt_node(std::ostringstream& os, const Assign& n, int d) {
        os << "{\"type\": \"Assign\", \"op\": \"" << escape(n.op) << "\"";
        os << ", \"name\": "; write_expr(os, *n.target, d);
        os << ", \"value\": "; write_expr(os, *n.value, d);
        os << "}";
    }

    static void write_params(std::ostringstream& os, const std::vector<Param>& params, int d) {
        os << "[";
        for (size_t i = 0; i < params.size(); i++) {
            if (i > 0) os << ", ";
            os << "{\"type\": \"Param\", \"name\": \"" << escape(params[i].name) << "\"";
            if (params[i].var_type) os << ", \"var_type\": \"" << escape(*params[i].var_type) << "\"";
            if (params[i].is_ref) os << ", \"is_ref\": true";
            os << "}";
        }
        os << "]";
    }

    static void write_stmt_node(std::ostringstream& os, const FuncDecl& n, int d) {
        os << "{\"type\": \"FuncDecl\", \"name\": \"" << escape(n.name) << "\"";
        os << ", \"params\": "; write_params(os, n.params, d);
        if (n.return_type) os << ", \"return_type\": \"" << escape(*n.return_type) << "\"";
        os << ", \"body\": "; write_stmts(os, n.body, d);
        os << "}";
    }

    static void write_struct_fields(std::ostringstream& os, const std::vector<StructField>& fields) {
        os << "[";
        for (size_t i = 0; i < fields.size(); i++) {
            if (i > 0) os << ", ";
            os << "{\"type\": \"StructField\", \"name\": \"" << escape(fields[i].name)
               << "\", \"var_type\": \"" << escape(fields[i].var_type) << "\"}";
        }
        os << "]";
    }

    static void write_stmt_node(std::ostringstream& os, const StructDecl& n, int d) {
        os << "{\"type\": \"StructDecl\", \"name\": \"" << escape(n.name) << "\"";
        os << ", \"fields\": "; write_struct_fields(os, n.fields);
        os << "}";
    }

    static void write_stmt_node(std::ostringstream& os, const EnumDecl& n, int) {
        os << "{\"type\": \"EnumDecl\", \"name\": \"" << escape(n.name) << "\", \"variants\": [";
        for (size_t i = 0; i < n.variants.size(); i++) {
            if (i > 0) os << ", ";
            os << "\"" << escape(n.variants[i]) << "\"";
        }
        os << "]}";
    }

    // Expression statement: write the bare node, write_stmt adds its position
    static void write_stmt_node(std::ostringstream& os, const Expr& e, int d) {
        std::visit([&](const auto& node) { write_expr_node(os, node, d); }, e.node);
    }

    // Catch-all for variant
    template<typename T>
    static void write_stmt_node(std::ostringstream& os, const T&, int) {
        os << "{}";
    }

    // ---- Expression writers ----

    static void write_expr_node(std::ostringstream& os, const Number& n, int) {
        os << "{\"type\": \"Number\", \"value\": " << n.value << "}";
    }

    static void write_expr_node(std::ostringstream& os, const FloatLit& n, int) {
        os << "{\"type\": \"Float\", \"value\": " << n.value << "}";
    }

    static void write_expr_node(std::ostringstream& os, const BoolLit& n, int) {
        os << "{\"type\": \"Bool\", \"value\": " << (n.value ? "true" : "false") << "}";
    }

    static void write_expr_node(std::ostringstream& os, const StringLit& n, int) {
        os << "{\"type\": \"String\", \"value\": \"" << escape(n.value) << "\"}";
    }

    static void write_expr_node(std::ostringstream& os, const Variable& n, int) {
        os << "{\"type\": \"Variable\", \"value\": \"" << escape(n.name) << "\"}";
    }

    static void write_expr_node(std::ostringstream& os, const ArrayLit& n, int d) {
        os << "{\"type\": \"ArrayLit\", \"elements\": "; write_exprs(os, n.elements, d); os << "}";
    }

    static void write_expr_node(std::ostringstream& os, const NewExpr& n, int d) {
        os << "{\"type\": \"New\", \"base\": \"" << escape(n.base_type) << "\"";
        os << ", \"size\": "; write_expr(os, *n.size, d); os << "}";
    }

    static void write_expr_node(std::ostringstream& os, const Call& n, int d) {
        os << "{\"type\": \"Call\", \"callee\": \"" << escape(n.callee) << "\"";
        os << ", \"args\": "; write_exprs(os, n.args, d); os << "}";
    }

    static void write_expr_node(std::ostringstream& os, const MemberAccess& n, int d) {
        os << "{\"type\": \"MemberAccess\", \"member\": \"" << escape(n.member) << "\"";
        os << ", \"object\": "; write_expr(os, *n.object, d); os << "}";
    }

    static void write_expr_node(std::ostringstream& os, const Index& n, int d) {
        os << "{\"type\": \"Index\", \"object\": "; write_expr(os, *n.object, d);
        os << ", \"index\": "; write_expr(os, *n.index, d); os << "}";
    }

    static void write_expr_node(std::ostringstream& os, const StructLiteral& n, int d) {
        os << "{\"type\": \"StructLiteral\", \"name\": \"" << escape(n.name) << "\"";
        os << ", \"args\": "; write_exprs(os, n.args, d); os << "}";
    }

    static void write_unary(std::ostringstream& os, const char* type, const ExprPtr& op, int d) {
        os << "{\"type\": \"" << type << "\", \"value\": "; write_expr(os, *op, d); os << "}";
    }

    static void write_expr_node(std::ostringstream& os, const UnaryMinus& n, int d) {
        write_unary(os, "Neg", n.operand, d);
    }

    static void write_expr_node(std::ostringstream& os, const LogicalNot& n, int d) {
        write_unary(os, "Not", n.operand, d);
    }

    static void write_binary(std::ostringstream& os, const char* type, const ExprPtr& l, const ExprPtr& r, int d) {
        os << "{\"type\": \"" << type << "\", \"left\": "; write_expr(os, *l, d);
        os << ", \"right\": "; write_expr(os, *r, d); os << "}";
    }

    static void write_expr_node(std::ostringstream& os, const Add& n, int d) { write_binary(os, "Add", n.left, n.right, d); }
    static void write_expr_node(std::ostringstream& os, const Sub& n, int d) { write_binary(os, "Sub", n.left, n.right, d); }
    static void write_expr_node(std::ostringstream& os, const Mul& n, int d) { write_binary(os, "Mul", n.left, n.right, d); }
    static void write_expr_node(std::ostringstream& os, const Div& n, int d) { write_binary(os, "Div", n.left, n.right, d); }
    static void write_expr_node(std::ostringstream& os, const And& n, int d) { write_binary(os, "And", n.left, n.right, d); }
    static void write_expr_node(std::ostringstream& os, const Or& n, int d)  { write_binary(os, "Or", n.left, n.right, d); }

    static void write_expr_node(std::ostringstream& os, const CmpOp& n, int d) {
        os << "{\"type\": \"CmpOp\", \"op\": \"" << escape(n.op) << "\"";
        os << ", \"left\": "; write_expr(os, *n.left, d);
        os << ", \"right\": "; write_expr(os, *n.right, d); os << "}";
    }

    // Catch-all
    template<typename T>
    static void write_expr_node(std::ostringstream& os, const T&, int) {
        os << "{}";
    }

    static std::string escape(const std::string& s) {
        std::string out;
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\t': out += "\\t"; break;
                case '\r': out += "\\r"; break;
                default:   out += c; break;
            }
        }
        return out;
    }
};

} // namespace aevix
