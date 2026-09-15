// ============================================================
// Aevix Sema — single source of truth for static checks.
//
// Consumes the frontend's ast.json (schema v1) and enforces the
// type/scoping rules of the memory model before codegen runs.
// Exit code: 0 on success, 1 when any diagnostic was emitted.
// ============================================================
#include "json_reader.hpp"

#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Declared symbols
// ---------------------------------------------------------------------------

struct SemaFn {
    std::vector<Param> params;
    std::string return_type; // "" for void functions
};

struct SemaStruct {
    std::vector<StructField> fields;
};

// ---------------------------------------------------------------------------
// The checker
// ---------------------------------------------------------------------------

class Checker {
public:
    explicit Checker(Program& program) : program_(program) {}

    bool run();

private:
    Program& program_;
    bool failed_ = false;

    std::map<std::string, SemaFn> funcs_;
    std::map<std::string, SemaStruct> structs_;

    // Scope stack. Top-level statements live at depth 0; every block,
    // function body, epoch, loop and if enters a new scope.
    std::vector<std::map<std::string, std::string>> scopes_;
    int epoch_depth_ = 0;
    // Variable -> epoch depth at which it was declared (-1 = no epoch).
    std::map<std::string, int> epoch_of_;

    const std::string* current_fn_ = nullptr;

    static bool is_primitive(const std::string& t) {
        return t == "int" || t == "float" || t == "bool" || t == "string";
    }

    static bool is_slice(const std::string& t) {
        return t.size() > 2 && t.compare(t.size() - 2, 2, "[]") == 0;
    }

    static bool is_fixed_array(const std::string& t) {
        return !is_slice(t) && t.find('[') != std::string::npos;
    }

    static std::string element_type(const std::string& t) {
        size_t b = t.find('[');
        return b == std::string::npos ? t : t.substr(0, b);
    }

    // True when `src` may be stored into a slot of type `dest`.
    bool assignable(const std::string& dest, const std::string& src);

    void error(const char* msg, int line, int col);
    std::string lookup_var(const std::string& name, int line, int col);
    void declare_var(const std::string& name, const std::string& type, int line, int col);

    std::string infer(Expr* e);
    std::string check_call(Call* c);
    void check_struct_lit(StructLiteral* sl);
    std::string bin_arith(Expr* l, Expr* r, int line, int col);

    // ref parameter helpers
    static bool is_lvalue(const Expr* e);
    static std::string lvalue_root(const Expr* e);

    // Epoch depth of the arena allocation a slice-typed expression refers to.
    // -1 when the expression holds no arena-referencing pointer.
    int arena_origin(Expr* e);
    int bind_depth_target(Expr* e);
    void check_escapes_target(Expr* target, Expr* value);

    void check_stmt(Stmt& s);
    void check_block(const std::vector<std::shared_ptr<Stmt>>& body);
    void check_epoch(Epoch& e);
};

// ---------------------------------------------------------------------------

bool Checker::assignable(const std::string& dest, const std::string& src) {
    if (src.empty()) return true; // the error is reported at the source
    if (dest == src) return true;
    // Widening: int -> float is the only implicit numeric conversion codegen supports.
    if (dest == "float" && src == "int") return true;
    // A slice may be written into a slice slot when element types agree
    // (array literals are coerced contextually, see infer()).
    if (is_slice(dest) && (is_slice(src) || is_fixed_array(src)))
        return element_type(dest) == element_type(src);
    return false;
}

void Checker::error(const char* msg, int line, int col) {
    failed_ = true;
    std::cerr << "error: " << msg;
    if (line > 0)
        std::cerr << " (line " << line << ", column " << col << ")";
    std::cerr << "\n";
}

std::string Checker::lookup_var(const std::string& name, int line, int col) {
    for (size_t i = scopes_.size(); i-- > 0;) {
        auto it = scopes_[i].find(name);
        if (it != scopes_[i].end()) return it->second;
    }
    error("undefined variable", line, col);
    return "";
}

void Checker::declare_var(const std::string& name, const std::string& type, int line, int col) {
    auto& top = scopes_.back();
    if (top.count(name)) {
        error("redeclaration of variable", line, col);
        return;
    }
    top[name] = type;
    epoch_of_[name] = epoch_depth_;
}

// ---------------------------------------------------------------------------
// Type inference
// ---------------------------------------------------------------------------

std::string Checker::infer(Expr* e) {
    if (auto* n = dynamic_cast<Number*>(e)) { (void)n; return "int"; }
    if (auto* f = dynamic_cast<Float*>(e)) { (void)f; return "float"; }
    if (auto* b = dynamic_cast<Bool*>(e)) { (void)b; return "bool"; }
    if (auto* s = dynamic_cast<String*>(e)) { (void)s; return "string"; }
    if (auto* v = dynamic_cast<Variable*>(e)) {
        return lookup_var(v->name, e->line, e->col);
    }
    if (auto* n = dynamic_cast<Neg*>(e)) {
        std::string t = infer(n->value.get());
        if (t != "int" && t != "float") error("negation requires a number", e->line, e->col);
        return t;
    }
    if (auto* un = dynamic_cast<Not*>(e)) {
        if (infer(un->value.get()) != "bool") error("! requires a bool", e->line, e->col);
        return "bool";
    }
    if (auto* a = dynamic_cast<Add*>(e))
        return bin_arith(a->left.get(), a->right.get(), e->line, e->col);
    if (auto* sub = dynamic_cast<Sub*>(e))
        return bin_arith(sub->left.get(), sub->right.get(), e->line, e->col);
    if (auto* m = dynamic_cast<Mul*>(e))
        return bin_arith(m->left.get(), m->right.get(), e->line, e->col);
    if (auto* d = dynamic_cast<Div*>(e))
        return bin_arith(d->left.get(), d->right.get(), e->line, e->col);
    if (auto* c = dynamic_cast<CmpOp*>(e)) {
        infer(c->left.get());
        infer(c->right.get());
        return "bool";
    }
    if (auto* an = dynamic_cast<And*>(e)) {
        if (infer(an->left.get()) != "bool" || infer(an->right.get()) != "bool")
            error("&& requires bools", e->line, e->col);
        return "bool";
    }
    if (auto* orr = dynamic_cast<Or*>(e)) {
        if (infer(orr->left.get()) != "bool" || infer(orr->right.get()) != "bool")
            error("|| requires bools", e->line, e->col);
        return "bool";
    }
    if (auto* ix = dynamic_cast<Index*>(e)) {
        std::string obj = infer(ix->object.get());
        std::string idx = infer(ix->index.get());
        if (obj.empty() || (!is_slice(obj) && !is_fixed_array(obj)))
            error("indexing a non-array value", e->line, e->col);
        if (idx != "int") error("array index must be an int", e->line, e->col);
        return element_type(obj);
    }
    if (auto* ma = dynamic_cast<MemberAccess*>(e)) {
        std::string obj = infer(ma->object.get());
        auto it = structs_.find(obj);
        if (it == structs_.end()) {
            error("member access on a non-struct value", e->line, e->col);
            return "";
        }
        for (const StructField& f : it->second.fields)
            if (f.name == ma->member) return f.var_type;
        error("unknown member", e->line, e->col);
        return "";
    }
    if (auto* sl = dynamic_cast<StructLiteral*>(e)) {
        check_struct_lit(sl);
        return sl->name;
    }
    if (auto* n = dynamic_cast<New*>(e)) {
        if (infer(n->size.get()) != "int")
            error("array size must be an int", e->line, e->col);
        if (!is_primitive(n->base) && !structs_.count(n->base))
            error("new of unknown element type", e->line, e->col);
        return n->base + "[]";
    }
    if (auto* al = dynamic_cast<ArrayLit*>(e)) {
        std::string elem;
        for (const auto& el : al->elements) {
            std::string t = infer(el.get());
            if (elem.empty()) elem = t;
            else if (t != elem && !assignable(elem, t) && !assignable(t, elem)) {
                error("array literal has mixed element types", e->line, e->col);
                break;
            }
        }
        return elem + "[]";
    }
    if (auto* c = dynamic_cast<Call*>(e))
        return check_call(c);
    error("could not determine expression type", e->line, e->col);
    return "";
}

std::string Checker::bin_arith(Expr* l, Expr* r, int line, int col) {
    std::string lt = infer(l), rt = infer(r);
    bool lnum = lt == "int" || lt == "float";
    bool rnum = rt == "int" || rt == "float";
    if (lnum && rnum) return (lt == "float" || rt == "float") ? "float" : "int";
    error("binary operator requires two numbers", line, col);
    return "";
}

// ---------------------------------------------------------------------------
// Escape analysis (memory model: a slice must not outlive its epoch)
// ---------------------------------------------------------------------------

int Checker::arena_origin(Expr* e) {
    if (auto* n = dynamic_cast<New*>(e))
        return epoch_depth_ > 0 ? epoch_depth_ : -1;
    if (auto* al = dynamic_cast<ArrayLit*>(e)) {
        int m = epoch_depth_ > 0 ? epoch_depth_ : -1;
        for (const auto& el : al->elements)
            m = std::max(m, arena_origin(el.get()));
        return m;
    }
    if (auto* v = dynamic_cast<Variable*>(e)) {
        std::string t = lookup_var(v->name, e->line, e->col);
        if (!is_slice(t) && !is_fixed_array(t)) return -1;
        auto it = epoch_of_.find(v->name);
        return it != epoch_of_.end() ? it->second : -1;
    }
    if (auto* ma = dynamic_cast<MemberAccess*>(e)) {
        if (!is_slice(infer(ma)) && !is_fixed_array(infer(ma))) return -1;
        return arena_origin(ma->object.get());
    }
    if (auto* ix = dynamic_cast<Index*>(e)) {
        if (!is_slice(infer(ix)) && !is_fixed_array(infer(ix))) return -1;
        return arena_origin(ix->object.get());
    }
    return -1;
}

int Checker::bind_depth_target(Expr* e) {
    if (auto* v = dynamic_cast<Variable*>(e)) {
        auto it = epoch_of_.find(v->name);
        return it != epoch_of_.end() ? it->second : -1;
    }
    if (auto* ix = dynamic_cast<Index*>(e))
        return bind_depth_target(ix->object.get());
    if (auto* ma = dynamic_cast<MemberAccess*>(e))
        return bind_depth_target(ma->object.get());
    return -1;
}

void Checker::check_escapes_target(Expr* target, Expr* value) {
    int origin = arena_origin(value);
    if (origin <= 0) return; // primitives are copied; file-scope values are safe
    int target_depth = bind_depth_target(target);
    if (target_depth < origin) {
        error("value assigned outside its epoch may be freed while referenced",
              value->line, value->col);
    }
}

// ---------------------------------------------------------------------------
// ref parameter helpers
// ---------------------------------------------------------------------------

bool Checker::is_lvalue(const Expr* e) {
    return dynamic_cast<const Variable*>(e) || dynamic_cast<const Index*>(e)
        || dynamic_cast<const MemberAccess*>(e);
}

std::string Checker::lvalue_root(const Expr* e) {
    if (auto* v = dynamic_cast<const Variable*>(e)) return v->name;
    if (auto* ix = dynamic_cast<const Index*>(e)) return lvalue_root(ix->object.get());
    if (auto* ma = dynamic_cast<const MemberAccess*>(e)) return lvalue_root(ma->object.get());
    return "";
}

// ---------------------------------------------------------------------------
// Statement walking
// ---------------------------------------------------------------------------

void Checker::check_struct_lit(StructLiteral* sl) {
    auto it = structs_.find(sl->name);
    if (it == structs_.end()) {
        error("unknown struct type", sl->line, sl->col);
        return;
    }
    const SemaStruct& st = it->second;
    if (st.fields.size() != sl->args.size()) {
        error("wrong number of arguments for struct literal", sl->line, sl->col);
        return;
    }
    for (size_t i = 0; i < st.fields.size(); ++i) {
        std::string t = infer(sl->args[i].get());
        if (!assignable(st.fields[i].var_type, t))
            error("struct literal field type mismatch", sl->line, sl->col);
    }
}

std::string Checker::check_call(Call* c) {
    auto it = funcs_.find(c->callee);
    if (it == funcs_.end()) {
        error("call to undefined function", c->line, c->col);
        return "";
    }
    const SemaFn& fn = it->second;
    if (fn.params.size() != c->args.size()) {
        error("wrong number of arguments in function call", c->line, c->col);
        return fn.return_type;
    }
    for (size_t i = 0; i < fn.params.size(); ++i) {
        const Param& p = fn.params[i];
        if (p.is_ref) {
            if (!is_lvalue(c->args[i].get()))
                error("ref parameter requires an lvalue argument", c->line, c->col);
        }
        std::string t = infer(c->args[i].get());
        if (!assignable(p.var_type, t))
            error("argument type mismatch in function call", c->line, c->col);
    }
    // Two ref parameters of one call must not alias the same variable:
    // the callee could otherwise write to one via the other.
    std::set<std::string> seen;
    for (size_t i = 0; i < fn.params.size(); ++i) {
        const Param& p = fn.params[i];
        if (!p.is_ref) continue;
        std::string root = lvalue_root(c->args[i].get());
        if (root.empty()) continue;
        if (!seen.insert(root).second)
            error("two ref parameters bound to the same variable", c->line, c->col);
    }
    return fn.return_type;
}

void Checker::check_epoch(Epoch& e) {
    epoch_depth_++;
    scopes_.push_back({});
    for (auto& s : e.body) check_stmt(*s);
    scopes_.pop_back();
    epoch_depth_--;
}

void Checker::check_block(const std::vector<std::shared_ptr<Stmt>>& body) {
    scopes_.push_back({});
    for (auto& s : body) check_stmt(*s);
    scopes_.pop_back();
}

void Checker::check_stmt(Stmt& s) {
    switch (s.kind) {
    case Stmt::Kind::Let: {
        std::string t = s.let.var_type;
        if (t.empty()) t = infer(s.let.value.get());
        else if (!assignable(t, infer(s.let.value.get())))
            error("let initialiser type mismatch", s.line, s.col);
        declare_var(s.let.name, t, s.line, s.col);
        break;
    }
    case Stmt::Kind::Epoch:
        check_epoch(s.epoch);
        break;
    case Stmt::Kind::Hot:
        check_block(s.hot.body);
        break;
    case Stmt::Kind::Print:
        for (auto& a : s.print.args) infer(a.get());
        break;
    case Stmt::Kind::If:
        infer(s.if_stmt->condition.get());
        check_block(s.if_stmt->then_block->body);
        if (s.if_stmt->else_block) check_block(s.if_stmt->else_block->body);
        break;
    case Stmt::Kind::While:
        infer(s.while_stmt->condition.get());
        check_block(s.while_stmt->body->body);
        break;
    case Stmt::Kind::For:
        scopes_.push_back({});
        if (s.for_stmt->init) check_stmt(*s.for_stmt->init);
        infer(s.for_stmt->condition.get());
        if (s.for_stmt->step) check_stmt(*s.for_stmt->step);
        check_block(s.for_stmt->body->body);
        scopes_.pop_back();
        break;
    case Stmt::Kind::ForIn: {
        scopes_.push_back({});
        std::string it = infer(s.for_in_stmt->iterable.get());
        if (it.empty() || (!is_slice(it) && !is_fixed_array(it)))
            error("for-in requires an array", s.line, s.col);
        declare_var(s.for_in_stmt->var, element_type(it), s.line, s.col);
        check_block(s.for_in_stmt->body->body);
        scopes_.pop_back();
        break;
    }
    case Stmt::Kind::Return: {
        if (!current_fn_) break; // top-level return not produced by the parser
        std::string ret_ty = s.return_stmt->value ? infer(s.return_stmt->value.get()) : "";
        auto it = funcs_.find(*current_fn_);
        if (it != funcs_.end() && !assignable(it->second.return_type, ret_ty))
            error("return value does not match function return type", s.line, s.col);
        if (epoch_depth_ > 0 && s.return_stmt->value) {
            int origin = arena_origin(s.return_stmt->value.get());
            if (origin >= epoch_depth_)
                error("returning a value allocated inside an epoch", s.line, s.col);
        }
        break;
    }
    case Stmt::Kind::Assign: {
        Assign* a = s.assign_stmt.get();
        std::string target_ty = infer(a->name.get());
        std::string value_ty = infer(a->value.get());
        if (a->op != "=" && target_ty != "int" && target_ty != "float")
            error("compound assignment requires a numeric target", s.line, s.col);
        if (!target_ty.empty() && !assignable(target_ty, value_ty))
            error("cannot assign value of incompatible type", s.line, s.col);
        check_escapes_target(a->name.get(), a->value.get());
        break;
    }
    case Stmt::Kind::FuncDecl: {
        scopes_.push_back({});
        SemaFn& fn = funcs_[s.func_decl->name];
        for (const Param& p : fn.params) {
            declare_var(p.name, p.var_type, s.line, s.col);
        }
        current_fn_ = &s.func_decl->name;
        check_block(s.func_decl->body->body);
        current_fn_ = nullptr;
        scopes_.pop_back();
        break;
    }
    case Stmt::Kind::CallStmt:
        infer(s.call_stmt.get());
        break;
    case Stmt::Kind::StructDecl:
        break; // collected in pass 1
    case Stmt::Kind::Break:
    case Stmt::Kind::Continue:
        break;
    }
}

// ---------------------------------------------------------------------------

bool Checker::run() {
    for (auto& s : program_.body) {
        switch (s->kind) {
        case Stmt::Kind::FuncDecl:
            for (const Param& p : s->func_decl->params)
                if (p.is_ref && is_slice(p.var_type))
                    error("open array parameters cannot be passed by reference", s->line, s->col);
            if (!funcs_.emplace(s->func_decl->name,
                        SemaFn{s->func_decl->params, s->func_decl->return_type}).second)
                error("redeclaration of function", s->line, s->col);
            break;
        case Stmt::Kind::StructDecl:
            if (!structs_.emplace(s->struct_decl->name,
                        SemaStruct{s->struct_decl->fields}).second)
                error("redeclaration of struct", s->line, s->col);
            break;
        default:
            break;
        }
    }
    scopes_.push_back({});
    for (auto& s : program_.body) check_stmt(*s);
    scopes_.pop_back();
    return !failed_;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: aevix-sema <ast.json>\n";
        return 2;
    }
    Program program = parse_json(argv[1]); // exits on parse/schema failure
    Checker checker(program);
    return checker.run() ? 0 : 1;
}