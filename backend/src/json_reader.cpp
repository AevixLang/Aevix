#include "json_reader.hpp"
#include <fstream>
#include <iostream>

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

    return nullptr;
}

static std::shared_ptr<Stmt> parse_stmt(const json& j);

static std::shared_ptr<Block> parse_block(const json& j) {
    auto block = std::make_shared<Block>();
    for (const auto& inner : j) {
        auto stmt = parse_stmt(inner);
        if (stmt) block->body.push_back(stmt);
    }
    return block;
}

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
            if (child && child->kind == Stmt::Kind::Let) {
                stmt->hot.body.push_back(child->let);
            }
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
