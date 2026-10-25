#pragma once
#include "ast.hpp"
#include "lock.hpp"
#include "parser.hpp"
#include "lexer.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <set>
#include <unordered_map>
#include <functional>

namespace aevix {

class ImportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ============================================================================
// Declaration renaming for namespaced imports
// ============================================================================
// A module imported as `alias` is flattened with every exported declaration
// renamed to `alias__name`; its internal references are rewritten the same way
// so it keeps calling its own symbols. The importing module's own
// `alias.name` references are rewritten afterwards from the exported map.

using RenameFn = std::function<std::string(const std::string&)>;

inline bool is_decl_stmt(const StmtPtr& s) {
    return std::holds_alternative<FuncDecl>(s->node) ||
           std::holds_alternative<StructDecl>(s->node) ||
           std::holds_alternative<EnumDecl>(s->node);
}

inline void rename_type(const RenameFn& fn, std::string& name) {
    if (name.empty()) return;
    size_t bracket = name.find('[');
    std::string base = bracket == std::string::npos ? name : name.substr(0, bracket);
    std::string rest = bracket == std::string::npos ? "" : name.substr(bracket);
    std::string mapped = fn(base);
    if (!mapped.empty()) name = mapped + rest;
}

inline void rename_type(const RenameFn& fn, std::optional<std::string>& name) {
    if (name) rename_type(fn, *name);
}

inline void rename_expr(ExprPtr& e, const RenameFn& fn);

inline void rename_expr_node(ExprVariant& node, const RenameFn& fn) {
    if (std::holds_alternative<Variable>(node)) {
        return; // bare values are never renamed: a local may shadow a decl
    } else if (auto* a = std::get_if<ArrayLit>(&node)) {
        for (auto& el : a->elements) rename_expr(el, fn);
    } else if (auto* n = std::get_if<NewExpr>(&node)) {
        rename_type(fn, n->base_type);
        rename_expr(n->size, fn);
    } else if (auto* c = std::get_if<Call>(&node)) {
        std::string mapped = fn(c->callee);
        if (!mapped.empty()) c->callee = mapped;
        for (auto& arg : c->args) rename_expr(arg, fn);
    } else if (auto* m = std::get_if<MemberAccess>(&node)) {
        if (auto* v = std::get_if<Variable>(&m->object->node)) {
            std::string qualified = v->name + "." + m->member;
            std::string mapped = fn(qualified);
            if (!mapped.empty()) {
                node = Variable{mapped};
                return;
            }
            std::string base = fn(v->name);
            if (!base.empty()) v->name = base;
            return;
        }
        rename_expr(m->object, fn);
    } else if (auto* i = std::get_if<Index>(&node)) {
        rename_expr(i->object, fn);
        rename_expr(i->index, fn);
    } else if (auto* s = std::get_if<StructLiteral>(&node)) {
        std::string mapped = fn(s->name);
        if (!mapped.empty()) s->name = mapped;
        for (auto& arg : s->args) rename_expr(arg, fn);
    } else if (auto* u = std::get_if<UnaryMinus>(&node)) {
        rename_expr(u->operand, fn);
    } else if (auto* l = std::get_if<LogicalNot>(&node)) {
        rename_expr(l->operand, fn);
    } else if (auto* b = std::get_if<Add>(&node)) {
        rename_expr(b->left, fn); rename_expr(b->right, fn);
    } else if (auto* b = std::get_if<Sub>(&node)) {
        rename_expr(b->left, fn); rename_expr(b->right, fn);
    } else if (auto* b = std::get_if<Mul>(&node)) {
        rename_expr(b->left, fn); rename_expr(b->right, fn);
    } else if (auto* b = std::get_if<Div>(&node)) {
        rename_expr(b->left, fn); rename_expr(b->right, fn);
    } else if (auto* b = std::get_if<Mod>(&node)) {
        rename_expr(b->left, fn); rename_expr(b->right, fn);
    } else if (auto* c = std::get_if<CmpOp>(&node)) {
        rename_expr(c->left, fn); rename_expr(c->right, fn);
    } else if (auto* b = std::get_if<And>(&node)) {
        rename_expr(b->left, fn); rename_expr(b->right, fn);
    } else if (auto* b = std::get_if<Or>(&node)) {
        rename_expr(b->left, fn); rename_expr(b->right, fn);
    }
}

inline void rename_expr(ExprPtr& e, const RenameFn& fn) {
    if (!e) return;
    rename_expr_node(e->node, fn);
}

inline void rename_stmts(std::vector<StmtPtr>& body, const RenameFn& fn);

inline void rename_stmt(StmtPtr& s, const RenameFn& fn) {
    if (auto* l = std::get_if<Let>(&s->node)) {
        rename_expr(l->value, fn);
        rename_type(fn, l->var_type);
    } else if (auto* h = std::get_if<Hot>(&s->node)) {
        rename_stmts(h->body, fn);
    } else if (auto* e = std::get_if<Epoch>(&s->node)) {
        rename_stmts(e->body, fn);
    } else if (auto* p = std::get_if<Print>(&s->node)) {
        for (auto& a : p->args) rename_expr(a, fn);
    } else if (auto* i = std::get_if<If>(&s->node)) {
        rename_expr(i->condition, fn);
        rename_stmts(i->then_body, fn);
        if (i->else_body) rename_stmts(*i->else_body, fn);
    } else if (auto* w = std::get_if<While>(&s->node)) {
        rename_expr(w->condition, fn);
        rename_stmts(w->body, fn);
    } else if (auto* f = std::get_if<For>(&s->node)) {
        if (f->init) rename_stmt(*f->init, fn);
        if (f->condition) rename_expr(*f->condition, fn);
        if (f->step) rename_stmt(*f->step, fn);
        rename_stmts(f->body, fn);
    } else if (auto* f = std::get_if<ForIn>(&s->node)) {
        rename_expr(f->iterable, fn);
        rename_stmts(f->body, fn);
    } else if (auto* r = std::get_if<Return>(&s->node)) {
        if (r->value) rename_expr(*r->value, fn);
    } else if (auto* a = std::get_if<Assign>(&s->node)) {
        rename_expr(a->target, fn);
        rename_expr(a->value, fn);
    } else if (auto* m = std::get_if<Match>(&s->node)) {
        rename_expr(m->scrutinee, fn);
        for (auto& c : m->cases) {
            rename_expr(c.value, fn);
            rename_stmts(c.body, fn);
        }
        if (m->default_body) rename_stmts(*m->default_body, fn);
    } else if (auto* f = std::get_if<FuncDecl>(&s->node)) {
        std::string mapped = fn(f->name);
        if (!mapped.empty()) f->name = mapped;
        for (auto& p : f->params) rename_type(fn, p.var_type);
        rename_type(fn, f->return_type);
        rename_stmts(f->body, fn);
    } else if (auto* st = std::get_if<StructDecl>(&s->node)) {
        std::string mapped = fn(st->name);
        if (!mapped.empty()) st->name = mapped;
        for (auto& field : st->fields) rename_type(fn, field.var_type);
    } else if (auto* e = std::get_if<EnumDecl>(&s->node)) {
        std::string mapped = fn(e->name);
        if (!mapped.empty()) e->name = mapped;
    } else if (auto* e = std::get_if<Expr>(&s->node)) {
        rename_expr_node(e->node, fn);
    }
}

inline void rename_stmts(std::vector<StmtPtr>& body, const RenameFn& fn) {
    for (auto& s : body) rename_stmt(s, fn);
}

class ImportResolver {
public:
    // Where `std:` resolves from. The toolchain sets this from its own
    // layout; leaving it empty means only $AEVIX_STDLIB is consulted.
    std::string stdlib_dir_;

    Program load(const std::string& path) {
        if (!std::filesystem::exists(path)) {
            throw ImportError("file not found: " + path);
        }
        std::string real_path = std::filesystem::canonical(path).string();
        std::vector<std::string> stack;
        std::set<std::string> loaded;
        std::set<const Stmt*> checked;
        std::unordered_map<std::string, std::pair<std::string, std::string>> origins; // name -> (kind, file)
        Program prog;
        prog.body = visit(real_path, stack, loaded, origins, checked, true);
        return prog;
    }

private:

    // prefix_module renames a module's exported declarations to `<alias>__<name>`
    // and rewrites the module's internal references, so it keeps calling its own
    // symbols after composition.
    void prefix_module(std::vector<StmtPtr>& decls, const std::string& alias,
                       std::unordered_map<std::string, std::string>& exported) {
        for (auto& s : decls) {
            if (auto* f = std::get_if<FuncDecl>(&s->node)) exported[f->name] = alias + "__" + f->name;
            else if (auto* st = std::get_if<StructDecl>(&s->node)) exported[st->name] = alias + "__" + st->name;
            else if (auto* e = std::get_if<EnumDecl>(&s->node)) exported[e->name] = alias + "__" + e->name;
        }
        RenameFn fn = [&](const std::string& n) -> std::string {
            auto it = exported.find(n);
            return it == exported.end() ? "" : it->second;
        };
        rename_stmts(decls, fn);
    }

    // rewrite_qualified resolves `alias.name` references in the importing
    // module, using the exported map recorded for that alias.
    void rewrite_qualified(std::vector<StmtPtr>& stmts,
        const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& nsmap) {
        if (nsmap.empty()) return;
        RenameFn fn = [&](const std::string& qualified) -> std::string {
            size_t dot = qualified.find('.');
            if (dot == std::string::npos) return "";
            std::string alias = qualified.substr(0, dot);
            std::string rest = qualified.substr(dot + 1);
            auto a = nsmap.find(alias);
            if (a == nsmap.end()) return "";
            auto m = a->second.find(rest);
            return m == a->second.end() ? "" : m->second;
        };
        rename_stmts(stmts, fn);
    }

    std::vector<StmtPtr> visit(const std::string& path, std::vector<std::string>& stack,
                               std::set<std::string>& loaded,
                               std::unordered_map<std::string, std::pair<std::string, std::string>>& origins,
                               std::set<const Stmt*>& checked, bool is_main) {
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

        Program prog = parse_file(real);
        check_nested_imports(prog, real);

        stack.push_back(real);
        std::vector<StmtPtr> own;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> nsmap;
        std::string dir = std::filesystem::path(real).parent_path().string();

        for (auto& s : prog.body) {
            if (auto* imp = std::get_if<Import>(&s->node)) {
                std::string sub = resolve_import(imp->spec, dir, real);
                std::string sub_real = std::filesystem::canonical(sub).string();
                if (imp->alias.empty()) {
                    if (loaded.count(sub_real)) continue;
                    std::vector<StmtPtr> decls = visit(sub, stack, loaded, origins, checked, false);
                    loaded.insert(sub_real);
                    for (auto& d : decls) {
                        check_name_conflict(d, sub_real, origins, checked);
                        own.push_back(d);
                    }
                } else {
                    std::vector<StmtPtr> decls = visit(sub, stack, loaded, origins, checked, false);
                    std::unordered_map<std::string, std::string> exported;
                    prefix_module(decls, imp->alias, exported);
                    nsmap[imp->alias] = exported;
                    for (auto& d : decls) {
                        check_name_conflict(d, sub_real, origins, checked);
                        own.push_back(d);
                    }
                }
            } else if (is_decl_stmt(s)) {
                // Imported declarations are checked by the caller after any
                // alias prefixing; only the main file's own decls are final here.
                if (is_main) check_name_conflict(s, real, origins, checked);
                own.push_back(s);
            } else if (is_main) {
                // top-level statements from the main file are kept
                own.push_back(s);
            }
        }

        stack.pop_back();
        rewrite_qualified(own, nsmap);
        return own;
    }
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

    void check_name_conflict(const StmtPtr& s, const std::string& file,
                             std::unordered_map<std::string, std::pair<std::string, std::string>>& origins,
                             std::set<const Stmt*>& checked) {
        // Nested levels hand the same node to every caller; validate it once.
        if (!checked.insert(s.get()).second) return;
        std::string name;
        std::string kind;
        if (auto* f = std::get_if<FuncDecl>(&s->node)) { name = f->name; kind = "FuncDecl"; }
        else if (auto* st = std::get_if<StructDecl>(&s->node)) { name = st->name; kind = "StructDecl"; }
        else if (auto* en = std::get_if<EnumDecl>(&s->node)) { name = en->name; kind = "EnumDecl"; }
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
            return resolve_std(trimmed.substr(4), importer);
        }

        if (trimmed.find(':') != std::string::npos) {
            return resolve_package(trimmed, from_dir, importer);
        }

        return resolve_path(trimmed, from_dir, importer);
    }

    // resolve_std maps `std:<module>` onto a file in the standard library.
    // The std ships *with* the toolchain rather than through aevix.lock: a user
    // never declares it as a dependency, and a version mismatch between the
    // compiler and its own std is not a state we can be in.
    std::string resolve_std(const std::string& module, const std::string& importer) {
        std::vector<std::string> bases;
        const char* env = std::getenv("AEVIX_STDLIB");
        if (env && *env) bases.push_back(env);
        if (!stdlib_dir_.empty()) bases.push_back(stdlib_dir_);

        for (auto& base : bases) {
            std::string cand = candidate(base, module);
            if (!cand.empty()) return cand;
        }

        std::string what = module.empty() ? "std" : ("std:" + module);
        throw ImportError("standard library module '" + what + "' not found (imported from " +
            displayName(importer) + "; searched " + std::to_string(bases.size()) +
            " location(s))");
    }

    std::string displayName(const std::string& p) {
        return std::filesystem::path(p).filename().string();
    }

    // find_lock walks up from `start` looking for aevix.lock, mirroring the
    // package manager's own project-root detection. Returns false if none is
    // found; outPath is then left untouched.
    bool find_lock(const std::string& start, std::string& outPath) {
        std::string d = std::filesystem::canonical(start).string();
        while (true) {
            std::string p = d + "/aevix.lock";
            if (std::filesystem::is_regular_file(p)) {
                outPath = p;
                return true;
            }
            auto parent = std::filesystem::path(d).parent_path().string();
            if (parent == d) return false;
            d = parent;
        }
    }

    // resolve_package implements the read-only half of the package contract:
    // it maps `name:module` onto a directory pinned in aevix.lock and resolves
    // the module inside it. The compiler never writes the lock — that is the
    // package manager's job.
    std::string resolve_package(const std::string& spec, const std::string& from_dir, const std::string& importer) {
        auto colon = spec.find(':');
        std::string name = spec.substr(0, colon);
        std::string module = spec.substr(colon + 1);

        std::string lockPath;
        if (!find_lock(from_dir, lockPath)) {
            throw ImportError("package '" + name + "' not found (imported from " +
                displayName(importer) + "): project has no aevix.lock; initialize it "
                "with the package manager (e.g. 'aevix-pm init')");
        }
        lock::Json data;
        try {
            data = lock::load(lockPath);
        } catch (const std::exception& e) {
            throw ImportError("cannot read aevix.lock: " + std::string(e.what()));
        }
        std::string lockdir = std::filesystem::path(lockPath).parent_path().string();

        const lock::Json* deps = data.get("dependencies");
        const lock::Json* dep = deps ? deps->get(name) : nullptr;
        if (!dep) {
            throw ImportError("package '" + name + "' is not in aevix.lock (imported from " +
                displayName(importer) + "); add it with the package manager (e.g. 'aevix-pm add " +
                name + "')");
        }

        std::string pkgdir;
        std::string path = dep->getString("path");
        if (!path.empty()) {
            // A relative path in the lock is relative to the lock's own directory.
            pkgdir = std::filesystem::path(path).is_absolute() ? path : (lockdir + "/" + path);
        } else {
            std::string version = dep->getString("version");
            if (version.empty()) {
                throw ImportError("package '" + name + "' in aevix.lock has no version or path");
            }
            const char* home = std::getenv("HOME");
            pkgdir = std::string(home ? home : "") + "/.aevix/pkg/" + name + "@" + version;
        }

        std::string cand = candidate(pkgdir, module);
        if (cand.empty()) {
            throw ImportError("module '" + module + "' not found in package '" + name +
                "' (imported from " + displayName(importer) + "; pkg dir: " + pkgdir + ")");
        }
        return cand;
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
