// ============================================================================
// Aevix Backend: JSON AST Reader
//
// This module implements the logic to read the AST exported by the frontend
// in JSON format and deserialize it into the C++ AST representation.
// ============================================================================
#include "json_reader.hpp"
#include <fstream>
#include <iostream>

/**
 * Recursively parses a JSON expression into a corresponding AST Expr node.
 */
static std::shared_ptr<Expr> parse_expr(const json& j) {
    if (!j.contains("type")) return nullptr;

    const std::string type = j["type"];

    if (type == "Number") {
        auto num = std::make_shared<Number>();
        num->value = j["value"];
        return num;
    }
    else if (type == "Float") {
        auto fl = std::make_shared<Float>();
        fl->value = j["value"];
        return fl;
    }
    else if (type == "Bool") {
        auto b = std::make_shared<Bool>();
        b->value = j["value"];
        return b;
    }
    else if (type == "String") {
        auto s = std::make_shared<String>();
        s->value = j["value"];
        return s;
    }
    else if (type == "Variable") {
        auto var = std::make_shared<Variable>();
        var->name = j["value"];
        return var;
    }
    else if (type == "ArrayLit") {
        auto al = std::make_shared<ArrayLit>();
        for (const auto& e : j["elements"]) {
            al->elements.push_back(parse_expr(e));
        }
        return al;
    }
    else if (type == "Index") {
        auto ix = std::make_shared<Index>();
        ix->object = parse_expr(j["object"]);
        ix->index = parse_expr(j["index"]);
        return ix;
    }
    else if (type == "Add") {
        auto add = std::make_shared<Add>();
        add->left = parse_expr(j["left"]);
        add->right = parse_expr(j["right"]);
        return add;
    }
    else if (type == "Sub") {
        auto sub = std::make_shared<Sub>();
        sub->left = parse_expr(j["left"]);
        sub->right = parse_expr(j["right"]);
        return sub;
    }
    else if (type == "Mul") {
        auto mul = std::make_shared<Mul>();
        mul->left = parse_expr(j["left"]);
        mul->right = parse_expr(j["right"]);
        return mul;
    }
    else if (type == "Div") {
        auto div = std::make_shared<Div>();
        div->left = parse_expr(j["left"]);
        div->right = parse_expr(j["right"]);
        return div;
    }
    else if (type == "Neg") {
        auto neg = std::make_shared<Neg>();
        neg->value = parse_expr(j["value"]);
        return neg;
    }
    else if (type == "CmpOp") {
        auto cmp = std::make_shared<CmpOp>();
        cmp->op = j["op"];
        cmp->left = parse_expr(j["left"]);
        cmp->right = parse_expr(j["right"]);
        return cmp;
    }
    else if (type == "And") {
        auto a = std::make_shared<And>();
        a->left = parse_expr(j["left"]);
        a->right = parse_expr(j["right"]);
        return a;
    }
    else if (type == "Or") {
        auto o = std::make_shared<Or>();
        o->left = parse_expr(j["left"]);
        o->right = parse_expr(j["right"]);
        return o;
    }
    else if (type == "Not") {
        auto n = std::make_shared<Not>();
        n->value = parse_expr(j["value"]);
        return n;
    }
    else if (type == "Call") {
        auto c = std::make_shared<Call>();
        c->callee = j["callee"];
        for (const auto& arg : j["args"]) {
            c->args.push_back(parse_expr(arg));
        }
        return c;
    }

    return nullptr;
}

/**
 * Parses a single JSON statement into a C++ Stmt object.
 */
static std::shared_ptr<Stmt> parse_stmt(const json& j);

/**
 * Parses a JSON block (list of statements) into a C++ Block object.
 */
static std::shared_ptr<Block> parse_block(const json& j) {
    auto block = std::make_shared<Block>();
    for (const auto& inner : j) {
        auto stmt = parse_stmt(inner);
        if (stmt) block->body.push_back(stmt);
    }
    return block;
}

/**
 * Parses a single JSON statement into a C++ Stmt object.
 */
static std::shared_ptr<Stmt> parse_stmt(const json& j) {
    if (!j.contains("type")) return nullptr;

    const std::string type = j["type"];
    auto stmt = std::make_shared<Stmt>();

    if (type == "Let") {
        stmt->kind = Stmt::Kind::Let;
        stmt->let.name = j["name"];
        stmt->let.value = parse_expr(j["value"]);
        if (j.contains("var_type") && !j["var_type"].is_null()) {
            stmt->let.var_type = j["var_type"];
        }
    }
    else if (type == "Hot") {
        stmt->kind = Stmt::Kind::Hot;
        for (const auto& inner : j["body"]) {
            auto child = parse_stmt(inner);
            if (child) stmt->hot.body.push_back(child);
        }
    }
    else if (type == "Print") {
        stmt->kind = Stmt::Kind::Print;
        stmt->print.value = parse_expr(j["value"]);
    }
    else if (type == "If") {
        stmt->kind = Stmt::Kind::If;
        auto if_stmt = std::make_shared<If>();
        if_stmt->condition = parse_expr(j["condition"]);
        if_stmt->then_block = parse_block(j["then_body"]);
        if (j.contains("else_body") && !j["else_body"].is_null()) {
            if_stmt->else_block = parse_block(j["else_body"]);
        }
        stmt->if_stmt = if_stmt;
    }
    else if (type == "While") {
        stmt->kind = Stmt::Kind::While;
        auto w = std::make_shared<While>();
        w->condition = parse_expr(j["condition"]);
        w->body = parse_block(j["body"]);
        stmt->while_stmt = w;
    }
    else if (type == "For") {
        stmt->kind = Stmt::Kind::For;
        auto f = std::make_shared<For>();
        if (j.contains("init") && !j["init"].is_null()) {
            f->init = parse_stmt(j["init"]);
        }
        if (j.contains("condition") && !j["condition"].is_null()) {
            f->condition = parse_expr(j["condition"]);
        }
        if (j.contains("step") && !j["step"].is_null()) {
            f->step = parse_stmt(j["step"]);
        }
        f->body = parse_block(j["body"]);
        stmt->for_stmt = f;
    }
    else if (type == "ForIn") {
        stmt->kind = Stmt::Kind::ForIn;
        auto fi = std::make_shared<ForIn>();
        fi->var = j["var"].get<std::string>();
        fi->iterable = parse_expr(j["iterable"]);
        fi->body = parse_block(j["body"]);
        stmt->for_in_stmt = fi;
    }
    else if (type == "Return") {
        stmt->kind = Stmt::Kind::Return;
        auto r = std::make_shared<Return>();
        if (j.contains("value") && !j["value"].is_null()) {
            r->value = parse_expr(j["value"]);
        }
        stmt->return_stmt = r;
    }
    else if (type == "Assign") {
        stmt->kind = Stmt::Kind::Assign;
        auto a = std::make_shared<Assign>();
        a->name = parse_expr(j["name"]);
        a->value = parse_expr(j["value"]);
        stmt->assign_stmt = a;
    }
    else if (type == "FuncDecl") {
        stmt->kind = Stmt::Kind::FuncDecl;
        auto fd = std::make_shared<FuncDecl>();
        fd->name = j["name"];
        for (const auto& p : j["params"]) {
            Param param;
            param.name = p["name"];
            if (p.contains("var_type") && !p["var_type"].is_null()) {
                param.var_type = p["var_type"];
            }
            fd->params.push_back(param);
        }
        if (j.contains("return_type") && !j["return_type"].is_null()) {
            fd->return_type = j["return_type"];
        }
        fd->body = parse_block(j["body"]);
        stmt->func_decl = fd;
    }
    else if (type == "Call") {
        stmt->kind = Stmt::Kind::CallStmt;
        stmt->call_stmt = std::dynamic_pointer_cast<Call>(parse_expr(j));
    }
    else {
        return nullptr;
    }

    return stmt;
}

Program parse_json(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "❌ Cannot open JSON file: " << filename << std::endl;
        exit(1);
    }

    json data;
    file >> data;

    Program program;
    for (const auto& stmt : data["body"]) {
        auto s = parse_stmt(stmt);
        if (s) program.body.push_back(s);
    }

    return program;
}
