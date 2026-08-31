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

    return nullptr;
}

static void parse_let(const json& j, Program& program) {
    Let let;
    let.name = j["name"];
    let.value = parse_expr(j["value"]);
    if (j.contains("var_type") && !j["var_type"].is_null()) {
        let.var_type = j["var_type"];
    }
    program.lets.push_back(let);
}

static void parse_hot(const json& j, Program& program) {
    Hot hot;
    for (const auto& inner : j["body"]) {
        if (inner["type"] == "Let") {
            Let let;
            let.name = inner["name"];
            let.value = parse_expr(inner["value"]);
            if (inner.contains("var_type") && !inner["var_type"].is_null()) {
                let.var_type = inner["var_type"];
            }
            hot.body.push_back(let);
        }
    }
    program.hots.push_back(hot);
}

static void parse_print(const json& j, Program& program) {
    Print print;
    print.value = parse_expr(j["value"]);
    program.prints.push_back(print);
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
        if (!stmt.contains("type")) continue;

        const std::string type = stmt["type"];

        if (type == "Let") {
            parse_let(stmt, program);
        }
        else if (type == "Hot") {
            parse_hot(stmt, program);
        }
        else if (type == "Print") {
            parse_print(stmt, program);
        }
    }

    return program;
}
