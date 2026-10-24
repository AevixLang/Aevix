// ============================================================================
// Aevix Frontend: aevix.lock reader
//
// The compiler is the read-only half of the package-management contract: it
// consumes aevix.lock (written by aevix-pm) to resolve `pkg:module` imports.
// The lock is JSON, so this header carries a small, self-contained JSON reader
// sized for that one file. It is deliberately not a general-purpose library —
// keeping it minimal means the consumer stays dependency-free and easy to keep
// in sync with the package manager. See notes/package-manager.md.
// ============================================================================
#pragma once
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace aevix {
namespace lock {

// Json is a tiny tagged value sufficient to walk the lock's shape: an object of
// strings (the dependency table). Other value kinds are parsed and discarded so
// a future lock field never breaks the reader.
struct Json {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    const Json* get(const std::string& key) const {
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
    std::string getString(const std::string& key, const std::string& fallback = "") const {
        const Json* v = get(key);
        return (v && v->type == String) ? v->str : fallback;
    }
};

class JsonParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Parser is a recursive-descent JSON reader. It owns the input text (rather
// than referencing it) so a caller may hand it a temporary string without the
// buffer dangling after construction.
class Parser {
public:
    explicit Parser(std::string text) : s_(std::move(text)), i_(0) {}

    Json parse() {
        Json v = parseValue();
        skipWs();
        if (i_ != s_.size()) throw JsonParseError("trailing characters in JSON");
        return v;
    }

private:
    std::string s_;
    size_t i_;

    void skipWs() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) i_++;
    }

    char peek() {
        if (i_ >= s_.size()) throw JsonParseError("unexpected end of JSON");
        return s_[i_];
    }

    void expect(char c) {
        if (i_ >= s_.size() || s_[i_] != c) throw JsonParseError(std::string("expected '") + c + "' in JSON");
        i_++;
    }

    Json parseValue() {
        skipWs();
        char c = peek();
        switch (c) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': { Json v; v.type = Json::String; v.str = parseString(); return v; }
            case 't': case 'f': return parseBool();
            case 'n': return parseNull();
            default: return parseNumber();
        }
    }

    Json parseObject() {
        Json v;
        v.type = Json::Object;
        expect('{');
        skipWs();
        if (peek() == '}') { i_++; return v; }
        while (true) {
            skipWs();
            std::string key = parseString();
            skipWs();
            expect(':');
            v.obj[key] = parseValue();
            skipWs();
            char c = peek();
            if (c == ',') { i_++; continue; }
            if (c == '}') { i_++; break; }
            throw JsonParseError("expected ',' or '}' in JSON object");
        }
        return v;
    }

    Json parseArray() {
        Json v;
        v.type = Json::Array;
        expect('[');
        skipWs();
        if (peek() == ']') { i_++; return v; }
        while (true) {
            v.arr.push_back(parseValue());
            skipWs();
            char c = peek();
            if (c == ',') { i_++; continue; }
            if (c == ']') { i_++; break; }
            throw JsonParseError("expected ',' or ']' in JSON array");
        }
        return v;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (true) {
            if (i_ >= s_.size()) throw JsonParseError("unterminated string in JSON");
            char c = s_[i_++];
            if (c == '"') break;
            if (c == '\\') {
                if (i_ >= s_.size()) throw JsonParseError("bad escape in JSON");
                char e = s_[i_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        // Keep it simple: drop the 4-hex-digit payload to a '?'
                        // rather than decoding UTF-16. Package names are ASCII.
                        if (i_ + 4 > s_.size()) throw JsonParseError("bad \\u escape in JSON");
                        i_ += 4;
                        out += '?';
                        break;
                    }
                    default: throw JsonParseError("unknown escape in JSON");
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    Json parseBool() {
        Json v;
        v.type = Json::Bool;
        if (s_.compare(i_, 4, "true") == 0) { v.b = true; i_ += 4; }
        else if (s_.compare(i_, 5, "false") == 0) { v.b = false; i_ += 5; }
        else throw JsonParseError("bad literal in JSON");
        return v;
    }

    Json parseNull() {
        if (s_.compare(i_, 4, "null") != 0) throw JsonParseError("bad literal in JSON");
        i_ += 4;
        return Json();
    }

    Json parseNumber() {
        Json v;
        v.type = Json::Number;
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) i_++;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' ||
                                  s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+'))
            i_++;
        if (i_ == start) throw JsonParseError("bad value in JSON");
        v.num = std::stod(s_.substr(start, i_ - start));
        return v;
    }
};

// load parses the aevix.lock file at `path` into a Json object.
inline Json load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) throw JsonParseError("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    Parser p(ss.str());
    Json v = p.parse();
    if (v.type != Json::Object) throw JsonParseError(path + ": lock root must be an object");
    return v;
}

} // namespace lock
} // namespace aevix
