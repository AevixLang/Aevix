#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>

namespace aevix {

enum class TokType {
    // Literals
    NUMBER, FLOAT, STRING, BOOL, FSTRING,
    // Identifier
    IDENT,
    // Type keywords
    T_INT, T_FLOAT, T_BOOL, T_STRING,
    T_I8, T_I16, T_I32, T_I64,
    T_U8, T_U16, T_U32, T_U64,
    T_F32, T_F64,
    // Keywords
    KW_FUNC, KW_LET, KW_HOT, KW_PRINT, KW_IF, KW_ELSE,
    KW_WHILE, KW_FOR, KW_IN, KW_RETURN, KW_BREAK, KW_CONTINUE,
    KW_STRUCT, KW_ENUM, KW_NEW, KW_IMPORT, KW_EPOCH, KW_REF,
    // Operators
    PLUS, MINUS, STAR, SLASH, MOD,
    EQ, PLUS_EQ, MINUS_EQ, STAR_EQ, SLASH_EQ, MOD_EQ,
    EQ_EQ, BANG_EQ, LT, GT, LT_EQ, GT_EQ,
    AMP_AMP, PIPE_PIPE, BANG,
    // Delimiters
    LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET,
    SEMI, COMMA, DOT, COLON,
    // Special
    EOF_TOKEN,
};

struct Token {
    TokType type;
    std::string value;
    int line;
    int col;
};

class Lexer {
public:
    Lexer(const std::string& source, const std::string& filename = "input.aev")
        : src(source), fname(filename), pos(0), line(1), col(1) {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        while (pos < src.size()) {
            skip_whitespace_and_comments();
            if (pos >= src.size()) break;
            tokens.push_back(next_token());
        }
        tokens.push_back({TokType::EOF_TOKEN, "", line, col});
        return tokens;
    }

private:
    std::string src;
    std::string fname;
    size_t pos;
    int line;
    int col;

    char peek() const { return pos < src.size() ? src[pos] : '\0'; }
    char peek2() const { return pos + 1 < src.size() ? src[pos + 1] : '\0'; }
    char advance() {
        char c = src[pos++];
        if (c == '\n') { line++; col = 1; }
        else { col++; }
        return c;
    }

    void skip_whitespace_and_comments() {
        while (pos < src.size()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                advance();
            } else if (c == '/' && peek2() == '/') {
                // line comment
                while (pos < src.size() && peek() != '\n') advance();
            } else if (c == '/' && peek2() == '*') {
                // block comment
                advance(); advance(); // skip /*
                while (pos < src.size()) {
                    if (peek() == '*' && peek2() == '/') {
                        advance(); advance();
                        break;
                    }
                    advance();
                }
            } else {
                break;
            }
        }
    }

    Token next_token() {
        char c = peek();
        int start_line = line;
        int start_col = col;

        // String literal
        if (c == '"') return read_string(start_line, start_col);

        // Interpolated string: an `f` immediately followed by `"` (no space) is
        // the f-string marker, matching Python. A lone `f` stays an identifier.
        if (c == 'f' && peek2() == '"') return read_fstring(start_line, start_col);

        // Number or float
        if (c >= '0' && c <= '9') return read_number(start_line, start_col);

        // Identifier or keyword
        if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            return read_ident(start_line, start_col);

        // Operators and delimiters
        return read_operator(start_line, start_col);
    }

    Token read_string(int sl, int sc) {
        advance(); // skip opening "
        std::string val;
        while (pos < src.size() && peek() != '"') {
            if (peek() == '\\') {
                advance();
                char esc = advance();
                switch (esc) {
                    case 'n': val += '\n'; break;
                    case 't': val += '\t'; break;
                    case 'r': val += '\r'; break;
                    case '\\': val += '\\'; break;
                    case '"': val += '"'; break;
                    default: val += '\\'; val += esc; break;
                }
            } else {
                val += advance();
            }
        }
        if (pos < src.size()) advance(); // skip closing "
        return {TokType::STRING, val, sl, sc};
    }

    // read_fstring consumes `f"..."`. It processes backslash escapes exactly
    // like a plain string but keeps `{`/`}` intact so the parser can split the
    // value into literal runs and embedded expressions.
    Token read_fstring(int sl, int sc) {
        advance(); // skip the f marker
        advance(); // skip opening "
        std::string val;
        while (pos < src.size() && peek() != '"') {
            if (peek() == '\\') {
                advance();
                char esc = advance();
                switch (esc) {
                    case 'n': val += '\n'; break;
                    case 't': val += '\t'; break;
                    case 'r': val += '\r'; break;
                    case '\\': val += '\\'; break;
                    case '"': val += '"'; break;
                    default: val += '\\'; val += esc; break;
                }
            } else {
                val += advance();
            }
        }
        if (pos < src.size()) advance(); // skip closing "
        return {TokType::FSTRING, val, sl, sc};
    }

    Token read_number(int sl, int sc) {
        std::string val;
        while (pos < src.size() && peek() >= '0' && peek() <= '9')
            val += advance();
        if (peek() == '.' && peek2() >= '0' && peek2() <= '9') {
            val += advance(); // .
            while (pos < src.size() && peek() >= '0' && peek() <= '9')
                val += advance();
            return {TokType::FLOAT, val, sl, sc};
        }
        return {TokType::NUMBER, val, sl, sc};
    }

    Token read_ident(int sl, int sc) {
        std::string val;
        while (pos < src.size()) {
            char c = peek();
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') {
                val += advance();
            } else break;
        }
        TokType type = classify_keyword(val);
        return {type, val, sl, sc};
    }

    TokType classify_keyword(const std::string& s) {
        if (s == "func")     return TokType::KW_FUNC;
        if (s == "let")      return TokType::KW_LET;
        if (s == "hot")      return TokType::KW_HOT;
        if (s == "print")    return TokType::KW_PRINT;
        if (s == "if")       return TokType::KW_IF;
        if (s == "else")     return TokType::KW_ELSE;
        if (s == "while")    return TokType::KW_WHILE;
        if (s == "for")      return TokType::KW_FOR;
        if (s == "in")       return TokType::KW_IN;
        if (s == "return")   return TokType::KW_RETURN;
        if (s == "break")    return TokType::KW_BREAK;
        if (s == "continue") return TokType::KW_CONTINUE;
        if (s == "struct")   return TokType::KW_STRUCT;
        if (s == "enum")     return TokType::KW_ENUM;
        if (s == "new")      return TokType::KW_NEW;
        if (s == "import")   return TokType::KW_IMPORT;
        if (s == "epoch")    return TokType::KW_EPOCH;
        if (s == "ref")      return TokType::KW_REF;
        if (s == "true")     return TokType::BOOL;
        if (s == "false")    return TokType::BOOL;
        if (s == "int")      return TokType::T_INT;
        if (s == "float")    return TokType::T_FLOAT;
        if (s == "bool")     return TokType::T_BOOL;
        if (s == "string")   return TokType::T_STRING;
        if (s == "i8")       return TokType::T_I8;
        if (s == "i16")      return TokType::T_I16;
        if (s == "i32")      return TokType::T_I32;
        if (s == "i64")      return TokType::T_I64;
        if (s == "u8")       return TokType::T_U8;
        if (s == "u16")      return TokType::T_U16;
        if (s == "u32")      return TokType::T_U32;
        if (s == "u64")      return TokType::T_U64;
        if (s == "f32")      return TokType::T_F32;
        if (s == "f64")      return TokType::T_F64;
        return TokType::IDENT;
    }

    Token read_operator(int sl, int sc) {
        char c = advance();
        switch (c) {
            case '+':
                if (peek() == '=') { advance(); return {TokType::PLUS_EQ, "+=", sl, sc}; }
                return {TokType::PLUS, "+", sl, sc};
            case '-':
                if (peek() == '=') { advance(); return {TokType::MINUS_EQ, "-=", sl, sc}; }
                return {TokType::MINUS, "-", sl, sc};
            case '*':
                if (peek() == '=') { advance(); return {TokType::STAR_EQ, "*=", sl, sc}; }
                return {TokType::STAR, "*", sl, sc};
            case '/':
                if (peek() == '=') { advance(); return {TokType::SLASH_EQ, "/=", sl, sc}; }
                return {TokType::SLASH, "/", sl, sc};
            case '%':
                if (peek() == '=') { advance(); return {TokType::MOD_EQ, "%=", sl, sc}; }
                return {TokType::MOD, "%", sl, sc};
            case '=':
                if (peek() == '=') { advance(); return {TokType::EQ_EQ, "==", sl, sc}; }
                return {TokType::EQ, "=", sl, sc};
            case '!':
                if (peek() == '=') { advance(); return {TokType::BANG_EQ, "!=", sl, sc}; }
                return {TokType::BANG, "!", sl, sc};
            case '<':
                if (peek() == '=') { advance(); return {TokType::LT_EQ, "<=", sl, sc}; }
                return {TokType::LT, "<", sl, sc};
            case '>':
                if (peek() == '=') { advance(); return {TokType::GT_EQ, ">=", sl, sc}; }
                return {TokType::GT, ">", sl, sc};
            case '&':
                if (peek() == '&') { advance(); return {TokType::AMP_AMP, "&&", sl, sc}; }
                break;
            case '|':
                if (peek() == '|') { advance(); return {TokType::PIPE_PIPE, "||", sl, sc}; }
                break;
            case '(': return {TokType::LPAREN, "(", sl, sc};
            case ')': return {TokType::RPAREN, ")", sl, sc};
            case '{': return {TokType::LBRACE, "{", sl, sc};
            case '}': return {TokType::RBRACE, "}", sl, sc};
            case '[': return {TokType::LBRACKET, "[", sl, sc};
            case ']': return {TokType::RBRACKET, "]", sl, sc};
            case ';': return {TokType::SEMI, ";", sl, sc};
            case ',': return {TokType::COMMA, ",", sl, sc};
            case '.': return {TokType::DOT, ".", sl, sc};
            case ':': return {TokType::COLON, ":", sl, sc};
        }
        throw std::runtime_error(std::string("Unexpected character '") + c + "'");
    }
};

} // namespace aevix
