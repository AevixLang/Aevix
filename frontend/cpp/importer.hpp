#pragma once
#include "ast.hpp"
#include "parser.hpp"
#include "lexer.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <set>
#include <unordered_map>

namespace aevix {

class ImportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ImportResolver {
public:
    Program load(const std::string& path) {
        if (!std::filesystem::exists(path)) {
            throw ImportError("file not found: " + path);
        }
        std::string real_path = std::filesystem::canonical(path).string();
        Program main_prog = parse_file(real_path);

        std::vector<StmtPtr> out;
        std::set<std::string> loaded;
        std::unordered_map<std::string, std::pair<std::string, std::string>> origins; // name -> (kind, file)
        std::vector<std::string> stack;

        for (auto& s : main_prog.body) {
            if (auto* imp = std::get_if<Import>(&s->node)) {
                std::string resolved = resolve_import(imp->spec, std::filesystem::path(real_path).parent_path().string(), real_path);
                visit(resolved, stack, loaded, out, origins);
            } else if (std::holds_alternative<FuncDecl>(s->node) || std::holds_alternative<StructDecl>(s->node)) {
                check_name_conflict(s, real_path, origins);
                out.push_back(s);
            } else {
                // top-level statements from main file are kept
                out.push_back(s);
            }
        }

        main_prog.body = out;
        return main_prog;
    }

private:
    Program parse_file(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            throw ImportError("Cannot open file: " + path);
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        std::string code = ss.str();

        Lexer lexer(code, path);
        auto tokens = lexer.tokenize();
        Parser parser(tokens, path);
        return parser.parse();
    }

    void visit(const std::string& path, std::vector<std::string>& stack,
               std::set<std::string>& loaded, std::vector<StmtPtr>& out,
               std::unordered_map<std::string, std::pair<std::string, std::string>>& origins) {
        std::string real = std::filesystem::canonical(path).string();

        // Circular check
        for (auto& s : stack) {
            if (s == real) {
                std::string chain;
                for (size_t i = stack.size() - 1; i < stack.size(); i--) {
                    if (!chain.empty()) chain += " -> ";
                    chain += std::filesystem::path(stack[i]).filename().string();
                }
                chain += " -> " + std::filesystem::path(real).filename().string();
                throw ImportError("circular import detected: " + chain);
            }
        }

        // Already loaded
        if (loaded.count(real)) return;

        Program prog = parse_file(real);
        check_nested_imports(prog, real);

        stack.push_back(real);
        for (auto& s : prog.body) {
            if (auto* imp = std::get_if<Import>(&s->node)) {
                std::string sub = resolve_import(imp->spec, std::filesystem::path(real).parent_path().string(), real);
                visit(sub, stack, loaded, out, origins);
            } else if (std::holds_alternative<FuncDecl>(s->node) || std::holds_alternative<StructDecl>(s->node)) {
                check_name_conflict(s, real, origins);
                out.push_back(s);
            }
        }
        stack.pop_back();
        loaded.insert(real);
    }

    void check_name_conflict(const StmtPtr& s, const std::string& file,
                             std::unordered_map<std::string, std::pair<std::string, std::string>>& origins) {
        std::string name;
        std::string kind;
        if (auto* f = std::get_if<FuncDecl>(&s->node)) { name = f->name; kind = "FuncDecl"; }
        else if (auto* st = std::get_if<StructDecl>(&s->node)) { name = st->name; kind = "StructDecl"; }
        if (name.empty()) return;

        auto it = origins.find(name);
        if (it != origins.end() && it->second.second != file) {
            throw ImportError("name conflict: '" + name + "' is defined in both '" +
                std::filesystem::path(it->second.second).filename().string() + "' and '" +
                std::filesystem::path(file).filename().string() + "'");
        }
        origins[name] = {kind, file};
    }

    void check_nested_imports(const Program& prog, const std::string& file) {
        for (auto& s : prog.body) {
            // Top-level imports are fine; check INSIDE statement bodies
            if (auto* f = std::get_if<FuncDecl>(&s->node)) {
                check_imports_in_body(f->body, file);
            } else if (auto* h = std::get_if<Hot>(&s->node)) {
                check_imports_in_body(h->body, file);
            } else if (auto* e = std::get_if<Epoch>(&s->node)) {
                check_imports_in_body(e->body, file);
            } else if (auto* i = std::get_if<If>(&s->node)) {
                check_imports_in_body(i->then_body, file);
                if (i->else_body) check_imports_in_body(*i->else_body, file);
            } else if (auto* w = std::get_if<While>(&s->node)) {
                check_imports_in_body(w->body, file);
            } else if (auto* fo = std::get_if<For>(&s->node)) {
                check_imports_in_body(fo->body, file);
            } else if (auto* fi = std::get_if<ForIn>(&s->node)) {
                check_imports_in_body(fi->body, file);
            }
        }
    }

    void check_imports_in_body(const std::vector<StmtPtr>& body, const std::string& file) {
        for (auto& s : body) {
            if (std::holds_alternative<Import>(s->node)) {
                throw ImportError("import is only allowed at the top level of a file: '" +
                    std::filesystem::path(file).filename().string() + "'");
            }
        }
    }

    std::string resolve_import(const std::string& spec, const std::string& from_dir, const std::string& importer) {
        std::string trimmed = spec;
        // trim whitespace
        while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();

        if (trimmed.substr(0, 4) == "std:") {
            throw ImportError("'std:' is reserved for the standard library (imported from " +
                std::filesystem::path(importer).filename().string() + ")");
        }

        if (trimmed.find(':') != std::string::npos) {
            throw ImportError("package imports not yet supported in C++ frontend");
        }

        return resolve_path(trimmed, from_dir, importer);
    }

    std::string resolve_path(const std::string& spec, const std::string& from_dir, const std::string& importer) {
        // Search: same dir, project lib/, then AEVIX_PATH
        std::vector<std::string> bases = {from_dir};

        // Find project root (walk up looking for aevix.toml or aevix.lock)
        std::string project = find_project_root(from_dir);
        bases.push_back(project + "/lib");

        const char* env = std::getenv("AEVIX_PATH");
        if (env) {
            std::istringstream ss(env);
            std::string entry;
            while (std::getline(ss, entry, ':')) {
                if (!entry.empty()) bases.push_back(entry);
            }
        }

        for (auto& base : bases) {
            std::string cand = candidate(base, spec);
            if (!cand.empty()) return cand;
        }

        throw ImportError("module '" + spec + "' not found (imported from " +
            std::filesystem::path(importer).filename().string() + ")");
    }

    std::string candidate(const std::string& base, const std::string& spec) {
        std::string p = base + "/" + spec;
        if (std::filesystem::is_regular_file(p)) return std::filesystem::canonical(p).string();
        if (!spec.ends_with(".aev") && std::filesystem::is_regular_file(p + ".aev"))
            return std::filesystem::canonical(p + ".aev").string();
        if (std::filesystem::is_directory(p) && std::filesystem::is_regular_file(p + "/lib.aev"))
            return std::filesystem::canonical(p + "/lib.aev").string();
        return "";
    }

    std::string find_project_root(const std::string& start) {
        std::string d = std::filesystem::canonical(start);
        while (true) {
            if (std::filesystem::is_regular_file(d + "/aevix.toml") ||
                std::filesystem::is_regular_file(d + "/aevix.lock"))
                return d;
            auto parent = std::filesystem::path(d).parent_path().string();
            if (parent == d) return std::filesystem::canonical(start);
            d = parent;
        }
    }
};

} // namespace aevix
