#pragma once
#include "ast.hpp"
#include "lexer.hpp"
#include <functional>

namespace aevix {

class Parser {
public:
    Parser(const std::vector<Token>& tokens, const std::string& filename = "input.aev")
        : tokens(tokens), pos(0), fname(filename) {}

    Program parse() {
        Program prog;
        while (!check(TokType::EOF_TOKEN)) {
            prog.body.push_back(parse_stmt());
        }
        return prog;
    }

    // parse_expression compiles a standalone expression string into an AST node.
    // f-string interpolation reuses this to expand each embedded `{...}` segment.
    static ExprPtr parse_expression(const std::string& code, const std::string& filename = "interp") {
        Lexer lex(code, filename);
        Parser sub(lex.tokenize(), filename);
        return sub.parse_expr();
    }

private:
    std::vector<Token> tokens;
    size_t pos;
    std::string fname;

    // ---- Token utilities ----

    Token& peek() { return tokens[pos]; }
    Token advance() { return tokens[pos++]; }

    bool check(TokType type) { return peek().type == type; }

    bool match(TokType type) {
        if (check(type)) { advance(); return true; }
        return false;
    }

    Token expect(TokType type, const char* msg) {
        if (check(type)) return advance();
        throw std::runtime_error(
            std::string(msg) + " at line " + std::to_string(peek().line) +
            ", column " + std::to_string(peek().col));
    }

    bool is_type_keyword(TokType t) {
        switch (t) {
            case TokType::T_INT: case TokType::T_FLOAT: case TokType::T_BOOL: case TokType::T_STRING:
            case TokType::T_I8: case TokType::T_I16: case TokType::T_I32: case TokType::T_I64:
            case TokType::T_U8: case TokType::T_U16: case TokType::T_U32: case TokType::T_U64:
            case TokType::T_F32: case TokType::T_F64:
                return true;
            default: return false;
        }
    }

    bool is_stmt_start(TokType t) {
        switch (t) {
            case TokType::KW_FUNC: case TokType::KW_LET: case TokType::KW_HOT:
            case TokType::KW_PRINT: case TokType::KW_IF: case TokType::KW_WHILE:
            case TokType::KW_FOR: case TokType::KW_RETURN: case TokType::KW_BREAK:
            case TokType::KW_CONTINUE: case TokType::KW_STRUCT: case TokType::KW_ENUM:
            case TokType::KW_IMPORT: case TokType::KW_EPOCH: case TokType::KW_MATCH:
                return true;
            default: return false;
        }
    }

    // ---- Type parsing ----

    std::string parse_type_name() {
        std::string name;
        if (is_type_keyword(peek().type)) {
            name = advance().value;
        } else if (check(TokType::IDENT)) {
            name = advance().value;
            // Qualified type from an aliased import: `ns.Type`. The importer
            // rewrites it to the prefixed declaration name before sema.
            while (check(TokType::DOT)) {
                advance();
                name += "." + expect(TokType::IDENT, "expected type name after '.'").value;
            }
        } else {
            throw std::runtime_error("expected type name");
        }
        // Check for array type: T[N]
        if (match(TokType::LBRACKET)) {
            std::string size_str;
            if (check(TokType::NUMBER)) {
                size_str = advance().value;
            }
            expect(TokType::RBRACKET, "expected ']'");
            return name + "[" + size_str + "]";
        }
        return name;
    }

    // ---- Statements ----

    StmtPtr parse_stmt() {
        int sl = peek().line, sc = peek().col;
        StmtPtr s;

        switch (peek().type) {
            case TokType::KW_IMPORT:  s = parse_import(); break;
            case TokType::KW_LET:     s = parse_let(); break;
            case TokType::KW_HOT:     s = parse_hot(); break;
            case TokType::KW_EPOCH:   s = parse_epoch(); break;
            case TokType::KW_PRINT:   s = parse_print(); break;
            case TokType::KW_IF:      s = parse_if(); break;
            case TokType::KW_WHILE:   s = parse_while(); break;
            case TokType::KW_FOR:     s = parse_for(); break;
            case TokType::KW_MATCH:   s = parse_match(); break;
            case TokType::KW_RETURN:  s = parse_return(); break;
            case TokType::KW_BREAK:   advance(); expect(TokType::SEMI, "expected ';'"); s = std::make_shared<Stmt>(Break{}); break;
            case TokType::KW_CONTINUE: advance(); expect(TokType::SEMI, "expected ';'"); s = std::make_shared<Stmt>(Continue{}); break;
            case TokType::KW_FUNC:    s = parse_func_decl(); break;
            case TokType::KW_STRUCT:  s = parse_struct_decl(); break;
            case TokType::KW_ENUM:    s = parse_enum_decl(); break;
            default: s = parse_expr_or_assign(); break;
        }
        s->line = sl;
        s->col = sc;
        return s;
    }

    StmtPtr parse_import() {
        advance(); // import
        Token spec = expect(TokType::STRING, "expected string after import");
        std::string alias = "";
        if (match(TokType::KW_AS)) {
            Token a = expect(TokType::IDENT, "expected identifier after 'as'");
            alias = a.value;
        }
        expect(TokType::SEMI, "expected ';'");
        return std::make_shared<Stmt>(Import{spec.value, alias});
    }

    StmtPtr parse_let() {
        advance(); // let
        Token name = expect(TokType::IDENT, "expected variable name");
        std::optional<std::string> type;
        if (match(TokType::COLON)) {
            type = parse_type_name();
        }
        expect(TokType::EQ, "expected '='");
        ExprPtr value = parse_expr();
        expect(TokType::SEMI, "expected ';'");
        return std::make_shared<Stmt>(Let{name.value, value, type});
    }

    StmtPtr parse_hot() {
        advance(); // hot
        expect(TokType::LBRACE, "expected '{'");
        auto body = parse_block();
        return std::make_shared<Stmt>(Hot{body});
    }

    StmtPtr parse_epoch() {
        advance(); // epoch
        expect(TokType::LBRACE, "expected '{'");
        auto body = parse_block();
        return std::make_shared<Stmt>(Epoch{body});
    }

    StmtPtr parse_print() {
        advance(); // print
        std::vector<ExprPtr> args;
        if (!check(TokType::SEMI)) {
            args.push_back(parse_expr());
            while (match(TokType::COMMA)) {
                args.push_back(parse_expr());
            }
        }
        expect(TokType::SEMI, "expected ';'");
        return std::make_shared<Stmt>(Print{args});
    }

    StmtPtr parse_if() {
        advance(); // if
        expect(TokType::LPAREN, "expected '('");
        ExprPtr cond = parse_expr();
        expect(TokType::RPAREN, "expected ')'");
        expect(TokType::LBRACE, "expected '{'");
        auto then_body = parse_block();
        std::optional<std::vector<StmtPtr>> else_body;
        if (match(TokType::KW_ELSE)) {
            if (check(TokType::KW_IF)) {
                auto elif = parse_if();
                else_body = std::vector<StmtPtr>{elif};
            } else {
                expect(TokType::LBRACE, "expected '{'");
                else_body = parse_block();
            }
        }
        return std::make_shared<Stmt>(If{cond, then_body, else_body});
    }

    StmtPtr parse_match() {
        advance(); // match
        expect(TokType::LPAREN, "expected '(' after match");
        ExprPtr scrutinee = parse_expr();
        expect(TokType::RPAREN, "expected ')'");
        expect(TokType::LBRACE, "expected '{'");
        std::vector<MatchCase> cases;
        std::optional<std::vector<StmtPtr>> default_body;
        while (!check(TokType::RBRACE)) {
            if (match(TokType::KW_CASE)) {
                ExprPtr val = parse_expr();
                expect(TokType::COLON, "expected ':' after case value");
                expect(TokType::LBRACE, "expected '{'");
                auto body = parse_block();
                cases.push_back(MatchCase{val, body});
            } else if (match(TokType::KW_DEFAULT)) {
                expect(TokType::COLON, "expected ':' after default");
                expect(TokType::LBRACE, "expected '{'");
                default_body = parse_block();
            } else {
                throw std::runtime_error("expected 'case' or 'default' in match");
            }
        }
        expect(TokType::RBRACE, "expected '}' after match");
        return std::make_shared<Stmt>(Match{scrutinee, cases, default_body});
    }

    ExprPtr parse_match_expr(int sl, int sc) {
        advance(); // match
        expect(TokType::LPAREN, "expected '(' after match");
        ExprPtr scrutinee = parse_expr();
        expect(TokType::RPAREN, "expected ')'");
        expect(TokType::LBRACE, "expected '{'");
        std::vector<MatchExprCase> cases;
        std::optional<ExprPtr> default_body;
        while (!check(TokType::RBRACE)) {
            if (match(TokType::KW_CASE)) {
                ExprPtr val = parse_expr();
                expect(TokType::ARROW, "expected '=>' after case value");
                ExprPtr body = parse_expr();
                cases.push_back(MatchExprCase{val, body});
            } else if (match(TokType::KW_DEFAULT)) {
                expect(TokType::ARROW, "expected '=>' after default");
                default_body = parse_expr();
            } else {
                throw std::runtime_error("expected 'case' or 'default' in match expression");
            }
            match(TokType::COMMA);
        }
        expect(TokType::RBRACE, "expected '}' after match expression");
        return std::make_shared<Expr>(Expr{MatchExpr{scrutinee, cases, default_body}, sl, sc});
    }

    StmtPtr parse_while() {
        advance(); // while
        expect(TokType::LPAREN, "expected '('");
        ExprPtr cond = parse_expr();
        expect(TokType::RPAREN, "expected ')'");
        expect(TokType::LBRACE, "expected '{'");
        auto body = parse_block();
        return std::make_shared<Stmt>(While{cond, body});
    }

    StmtPtr parse_for() {
        advance(); // for
        // for-in: for NAME in EXPR BLOCK
        if (check(TokType::IDENT) && pos + 1 < tokens.size() && tokens[pos + 1].type == TokType::KW_IN) {
            Token var = advance(); // name
            advance(); // in
            ExprPtr iterable = parse_expr_no_brace();
            expect(TokType::LBRACE, "expected '{'");
            auto body = parse_block();
            return std::make_shared<Stmt>(ForIn{var.value, iterable, body});
        }
        expect(TokType::LPAREN, "expected '('");
        std::optional<StmtPtr> init;
        std::optional<ExprPtr> cond;
        std::optional<StmtPtr> step;

        // init
        if (!check(TokType::SEMI)) {
            if (check(TokType::KW_LET)) {
                init = parse_let_no_semi();
            } else {
                ExprPtr e = parse_expr();
                if (check(TokType::EQ) || check(TokType::PLUS_EQ) || check(TokType::MINUS_EQ) ||
                    check(TokType::STAR_EQ) || check(TokType::SLASH_EQ)) {
                    std::string op = advance().value;
                    ExprPtr rhs = parse_expr();
                    init = std::make_shared<Stmt>(Assign{e, op, rhs});
                } else {
                    init = std::make_shared<Stmt>(*e);
                }
            }
        }
        expect(TokType::SEMI, "expected ';'");

        // condition
        if (!check(TokType::SEMI)) {
            cond = parse_expr();
        }
        expect(TokType::SEMI, "expected ';'");

        // step
        if (!check(TokType::RPAREN)) {
            // step is an assignment or expression
            ExprPtr step_expr = parse_expr();
            // Check if it's an assignment: expr (=|+=|-=|*=|/=) expr
            if (check(TokType::EQ) || check(TokType::PLUS_EQ) || check(TokType::MINUS_EQ) ||
                check(TokType::STAR_EQ) || check(TokType::SLASH_EQ)) {
                std::string op = advance().value;
                ExprPtr rhs = parse_expr();
                step = std::make_shared<Stmt>(Assign{step_expr, op, rhs});
            } else {
                step = std::make_shared<Stmt>(*step_expr);
            }
        }
        expect(TokType::RPAREN, "expected ')'");
        expect(TokType::LBRACE, "expected '{'");
        auto body = parse_block();
        return std::make_shared<Stmt>(For{init, cond, step, body});
    }

    StmtPtr parse_return() {
        advance(); // return
        std::optional<ExprPtr> value;
        if (!check(TokType::SEMI)) {
            value = parse_expr();
        }
        expect(TokType::SEMI, "expected ';'");
        return std::make_shared<Stmt>(Return{value});
    }

    StmtPtr parse_func_decl() {
        advance(); // func
        Token name = expect(TokType::IDENT, "expected function name");
        expect(TokType::LPAREN, "expected '('");
        std::vector<Param> params;
        if (!check(TokType::RPAREN)) {
            params.push_back(parse_param());
            while (match(TokType::COMMA)) {
                params.push_back(parse_param());
            }
        }
        expect(TokType::RPAREN, "expected ')'");
        std::optional<std::string> ret_type;
        if (match(TokType::COLON)) {
            ret_type = parse_type_name();
        }
        expect(TokType::LBRACE, "expected '{'");
        auto body = parse_block();
        return std::make_shared<Stmt>(FuncDecl{name.value, params, ret_type, body});
    }

    Param parse_param() {
        Token name = expect(TokType::IDENT, "expected parameter name");
        bool is_ref = false;
        std::optional<std::string> type;
        if (match(TokType::COLON)) {
            if (match(TokType::KW_REF)) {
                is_ref = true;
            }
            type = parse_type_name();
        }
        return Param{name.value, type, is_ref};
    }

    StmtPtr parse_struct_decl() {
        advance(); // struct
        Token name = expect(TokType::IDENT, "expected struct name");
        expect(TokType::LBRACE, "expected '{'");
        std::vector<StructField> fields;
        if (!check(TokType::RBRACE)) {
            fields.push_back(parse_struct_field());
            while (match(TokType::COMMA)) {
                if (check(TokType::RBRACE)) break; // trailing comma
                fields.push_back(parse_struct_field());
            }
        }
        expect(TokType::RBRACE, "expected '}'");
        return std::make_shared<Stmt>(StructDecl{name.value, fields});
    }

    StructField parse_struct_field() {
        Token name = expect(TokType::IDENT, "expected field name");
        expect(TokType::COLON, "expected ':'");
        std::string type = parse_type_name();
        return StructField{name.value, type};
    }

    StmtPtr parse_enum_decl() {
        advance(); // enum
        Token name = expect(TokType::IDENT, "expected enum name");
        expect(TokType::LBRACE, "expected '{'");
        std::vector<std::string> variants;
        if (!check(TokType::RBRACE)) {
            variants.push_back(expect(TokType::IDENT, "expected variant").value);
            while (match(TokType::COMMA)) {
                if (check(TokType::RBRACE)) break;
                variants.push_back(expect(TokType::IDENT, "expected variant").value);
            }
        }
        expect(TokType::RBRACE, "expected '}'");
        return std::make_shared<Stmt>(EnumDecl{name.value, variants});
    }

    StmtPtr parse_expr_or_assign() {
        ExprPtr expr = parse_expr();
        // Check for assignment
        if (check(TokType::EQ) || check(TokType::PLUS_EQ) || check(TokType::MINUS_EQ) ||
            check(TokType::STAR_EQ) || check(TokType::SLASH_EQ)) {
            std::string op = advance().value;
            ExprPtr value = parse_expr();
            expect(TokType::SEMI, "expected ';'");
            return std::make_shared<Stmt>(Assign{expr, op, value});
        }
        // Expression statement
        expect(TokType::SEMI, "expected ';'");
        return std::make_shared<Stmt>(*expr);
    }

    StmtPtr parse_let_no_semi() {
        advance(); // let
        Token name = expect(TokType::IDENT, "expected variable name");
        std::optional<std::string> type;
        if (match(TokType::COLON)) {
            type = parse_type_name();
        }
        expect(TokType::EQ, "expected '='");
        ExprPtr value = parse_expr();
        return std::make_shared<Stmt>(Let{name.value, value, type});
    }

    std::vector<StmtPtr> parse_block() {
        std::vector<StmtPtr> stmts;
        while (!check(TokType::RBRACE) && !check(TokType::EOF_TOKEN)) {
            stmts.push_back(parse_stmt());
        }
        expect(TokType::RBRACE, "expected '}'");
        return stmts;
    }

    // ---- Expressions (precedence climbing) ----

    ExprPtr parse_expr() { return parse_or(false); }
    ExprPtr parse_expr_no_brace() { return parse_or(true); }

    // build_fstring turns an f-string's raw value into a concatenation of
    // string literals and to_str() calls. `{{` and `}}` escape a literal brace;
    // a single `{`...`}` holds an expression, parsed recursively. Desugaring at
    // parse time keeps sema and codegen unchanged (both already understand `+`
    // on strings and to_str).
    ExprPtr build_fstring(const std::string& raw, int sl, int sc) {
        std::vector<ExprPtr> pieces;
        std::string lit;

        auto flush_lit = [&]() {
            if (!lit.empty()) {
                pieces.push_back(std::make_shared<Expr>(Expr{StringLit{lit}, sl, sc}));
                lit.clear();
            }
        };

        size_t i = 0;
        while (i < raw.size()) {
            char c = raw[i];
            if (c == '{' && i + 1 < raw.size() && raw[i + 1] == '{') {
                lit += '{'; i += 2; continue;
            }
            if (c == '}' && i + 1 < raw.size() && raw[i + 1] == '}') {
                lit += '}'; i += 2; continue;
            }
            if (c == '{') {
                size_t close = raw.find('}', i + 1);
                if (close == std::string::npos)
                    throw std::runtime_error("unterminated { in f-string");
                std::string code = raw.substr(i + 1, close - i - 1);
                flush_lit();
                // The empty expression `f"{}"` is a mistake, not an empty value.
                if (code.find_first_not_of(" \t") == std::string::npos)
                    throw std::runtime_error("empty {} in f-string");
                ExprPtr sub = parse_expression(code, fname);
                pieces.push_back(std::make_shared<Expr>(
                    Expr{Call{"to_str", {sub}}, sub->line, sub->col}));
                i = close + 1; continue;
            }
            lit += c; i++;
        }
        flush_lit();

        // Fold the pieces into a left-leaning `+` chain.
        if (pieces.empty()) return std::make_shared<Expr>(Expr{StringLit{""}, sl, sc});
        ExprPtr acc = pieces[0];
        for (size_t k = 1; k < pieces.size(); k++)
            acc = std::make_shared<Expr>(Expr{Add{acc, pieces[k]}, sl, sc});
        return acc;
    }

    ExprPtr parse_or(bool stop_before_brace) {
        ExprPtr left = parse_and(stop_before_brace);
        while (match(TokType::PIPE_PIPE)) {
            ExprPtr right = parse_and(stop_before_brace);
            auto node = Or{left, right};
            left = std::make_shared<Expr>(Expr{node, left->line, left->col});
        }
        return left;
    }

    ExprPtr parse_and(bool stop_before_brace) {
        ExprPtr left = parse_comparison(stop_before_brace);
        while (match(TokType::AMP_AMP)) {
            ExprPtr right = parse_comparison(stop_before_brace);
            auto node = And{left, right};
            left = std::make_shared<Expr>(Expr{node, left->line, left->col});
        }
        return left;
    }

    ExprPtr parse_comparison(bool stop_before_brace) {
        ExprPtr left = parse_add(stop_before_brace);
        while (check(TokType::EQ_EQ) || check(TokType::BANG_EQ) ||
               check(TokType::LT) || check(TokType::GT) ||
               check(TokType::LT_EQ) || check(TokType::GT_EQ)) {
            std::string op = advance().value;
            ExprPtr right = parse_add(stop_before_brace);
            auto node = CmpOp{op, left, right};
            left = std::make_shared<Expr>(Expr{node, left->line, left->col});
        }
        return left;
    }

    ExprPtr parse_add(bool stop_before_brace) {
        ExprPtr left = parse_mul(stop_before_brace);
        while (check(TokType::PLUS) || check(TokType::MINUS)) {
            bool is_add = check(TokType::PLUS);
            advance();
            ExprPtr right = parse_mul(stop_before_brace);
            if (is_add) {
                auto node = Add{left, right};
                left = std::make_shared<Expr>(Expr{node, left->line, left->col});
            } else {
                auto node = Sub{left, right};
                left = std::make_shared<Expr>(Expr{node, left->line, left->col});
            }
        }
        return left;
    }

    ExprPtr parse_mul(bool stop_before_brace) {
        ExprPtr left = parse_factor(stop_before_brace);
        while (check(TokType::STAR) || check(TokType::SLASH) || check(TokType::MOD)) {
            TokType op = peek().type;
            advance();
            ExprPtr right = parse_factor(stop_before_brace);
            if (op == TokType::STAR) {
                auto node = Mul{left, right};
                left = std::make_shared<Expr>(Expr{node, left->line, left->col});
            } else if (op == TokType::SLASH) {
                auto node = Div{left, right};
                left = std::make_shared<Expr>(Expr{node, left->line, left->col});
            } else {
                auto node = Mod{left, right};
                left = std::make_shared<Expr>(Expr{node, left->line, left->col});
            }
        }
        return left;
    }

    ExprPtr parse_factor(bool stop_before_brace) {
        int sl = peek().line, sc = peek().col;
        if (match(TokType::MINUS)) {
            ExprPtr operand = parse_factor(stop_before_brace);
            return std::make_shared<Expr>(Expr{UnaryMinus{operand}, sl, sc});
        }
        if (match(TokType::BANG)) {
            ExprPtr operand = parse_factor(stop_before_brace);
            return std::make_shared<Expr>(Expr{LogicalNot{operand}, sl, sc});
        }
        return parse_atom(stop_before_brace);
    }

    ExprPtr parse_atom(bool stop_before_brace) {
        int sl = peek().line, sc = peek().col;

        // match expression: match (x) { case 1 => 10, default => 0 }
        if (check(TokType::KW_MATCH)) {
            return parse_match_expr(sl, sc);
        }

        // Parenthesized expression
        if (match(TokType::LPAREN)) {
            ExprPtr e = parse_expr();
            expect(TokType::RPAREN, "expected ')'");
            return e;
        }

        // new T[n]
        if (check(TokType::KW_NEW)) {
            advance();
            std::string base;
            if (is_type_keyword(peek().type)) {
                base = advance().value;
            } else {
                base = expect(TokType::IDENT, "expected type name").value;
                while (match(TokType::DOT)) {
                    base += "." + expect(TokType::IDENT, "expected type name after '.'").value;
                }
            }
            expect(TokType::LBRACKET, "expected '['");
            ExprPtr size = parse_expr();
            expect(TokType::RBRACKET, "expected ']'");
            return std::make_shared<Expr>(Expr{NewExpr{base, size}, sl, sc});
        }

        // Array literal
        if (match(TokType::LBRACKET)) {
            std::vector<ExprPtr> elems;
            if (!check(TokType::RBRACKET)) {
                elems.push_back(parse_expr());
                while (match(TokType::COMMA)) {
                    elems.push_back(parse_expr());
                }
            }
            expect(TokType::RBRACKET, "expected ']'");
            return std::make_shared<Expr>(Expr{ArrayLit{elems}, sl, sc});
        }

        // Literals
        if (check(TokType::NUMBER)) {
            Token t = advance();
            return std::make_shared<Expr>(Expr{Number{std::stoll(t.value)}, sl, sc});
        }
        if (check(TokType::FLOAT)) {
            Token t = advance();
            return std::make_shared<Expr>(Expr{FloatLit{std::stod(t.value)}, sl, sc});
        }
        if (check(TokType::BOOL)) {
            Token t = advance();
            return std::make_shared<Expr>(Expr{BoolLit{t.value == "true"}, sl, sc});
        }
        if (check(TokType::STRING)) {
            Token t = advance();
            return std::make_shared<Expr>(Expr{StringLit{t.value}, sl, sc});
        }
        if (check(TokType::FSTRING)) {
            Token t = advance();
            return build_fstring(t.value, sl, sc);
        }

        // Primary: identifier with optional suffixes.
        // Struct literal: NAME { ... }, where NAME may be qualified by an
        // import alias (`ns.Point { ... }`).
        if (!stop_before_brace && check(TokType::IDENT)) {
            size_t q = pos;
            std::string lit_name = tokens[q].value;
            q += 1;
            while (q + 1 < tokens.size() && tokens[q].type == TokType::DOT &&
                   tokens[q + 1].type == TokType::IDENT) {
                lit_name += "." + tokens[q + 1].value;
                q += 2;
            }
            if (q < tokens.size() && tokens[q].type == TokType::LBRACE) {
                pos = q + 1; // consume the name and '{'
                std::vector<ExprPtr> args;
                std::vector<std::string> field_names;
                if (!check(TokType::RBRACE)) {
                    while (true) {
                        if (check(TokType::IDENT) && pos + 1 < tokens.size()
                            && tokens[pos + 1].type == TokType::COLON) {
                            Token fld = advance();
                            expect(TokType::COLON, "expected ':'");
                            args.push_back(parse_expr());
                            field_names.push_back(fld.value);
                        } else {
                            args.push_back(parse_expr());
                            field_names.push_back("");
                        }
                        if (!match(TokType::COMMA)) break;
                        if (check(TokType::RBRACE)) break;
                    }
                }
                expect(TokType::RBRACE, "expected '}'");
                return std::make_shared<Expr>(Expr{StructLiteral{lit_name, args, field_names}, sl, sc});
            }
        }
        return parse_primary();
    }

    ExprPtr parse_primary() {
        int sl = peek().line, sc = peek().col;

        Token name = expect(TokType::IDENT, "expected identifier");
        ExprPtr result = std::make_shared<Expr>(Expr{Variable{name.value}, sl, sc});
        return parse_suffixes(result);
    }

    // qualified_name flattens `a`, `a.b`, `a.b.c` into a dotted string; used
    // only to recover a call target from a member-access base.
    static std::string qualified_name(const ExprPtr& e) {
        if (auto* v = std::get_if<Variable>(&e->node)) return v->name;
        if (auto* m = std::get_if<MemberAccess>(&e->node)) {
            std::string base = qualified_name(m->object);
            if (base.empty()) return "";
            return base + "." + m->member;
        }
        return "";
    }

    ExprPtr parse_suffixes(ExprPtr base) {
        while (true) {
            int sl = peek().line, sc = peek().col;
            if (check(TokType::LPAREN)) {
                // Call. A qualified callee (`ns.fn`) keeps the full name so the
                // import resolver can map it onto the aliased declaration.
                advance();
                std::string callee = qualified_name(base);
                std::vector<ExprPtr> args;
                if (!check(TokType::RPAREN)) {
                    args.push_back(parse_expr());
                    while (match(TokType::COMMA)) {
                        args.push_back(parse_expr());
                    }
                }
                expect(TokType::RPAREN, "expected ')'");
                base = std::make_shared<Expr>(Expr{Call{callee, args}, sl, sc});
            } else if (check(TokType::LBRACKET)) {
                // Index
                advance();
                ExprPtr idx = parse_expr();
                expect(TokType::RBRACKET, "expected ']'");
                base = std::make_shared<Expr>(Expr{Index{base, idx}, sl, sc});
            } else if (check(TokType::DOT)) {
                // Member access
                advance();
                Token member = expect(TokType::IDENT, "expected member name");
                base = std::make_shared<Expr>(Expr{MemberAccess{base, member.value}, sl, sc});
            } else {
                break;
            }
        }
        return base;
    }
};

} // namespace aevix
