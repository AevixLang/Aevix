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
    std::set<int> escaping_params; // indices of params that escape to outer scope
};

struct SemaStruct {
    std::vector<StructField> fields;
};

// ---------------------------------------------------------------------------
// Type classification for fixed-width types
//
// Numeric types carry an implicit width and signedness. "int" and "float" are
// legacy aliases for "i32" and "f64" that behave identically in every rule.
// ---------------------------------------------------------------------------

static bool is_signed_int(const std::string& t) {
    return t == "int" || t == "i8" || t == "i16" || t == "i32" || t == "i64";
}

static bool is_unsigned_int(const std::string& t) {
    return t == "u8" || t == "u16" || t == "u32" || t == "u64";
}

static bool is_int_type(const std::string& t) {
    return is_signed_int(t) || is_unsigned_int(t);
}

static bool is_float_type(const std::string& t) {
    return t == "float" || t == "f32" || t == "f64";
}

static bool is_numeric(const std::string& t) {
    return is_int_type(t) || is_float_type(t);
}

// Width in bits: int == i32, float == f64.
static int int_bits(const std::string& t) {
    if (t == "i16" || t == "u16") return 16;
    if (t == "i32" || t == "u32" || t == "int") return 32;
    if (t == "i64" || t == "u64") return 64;
    return 8; // i8/u8
}

static int float_bits(const std::string& t) {
    return t == "f32" ? 32 : 64; // float/f64
}

// The result type of an arithmetic op, or "" when the operand kinds clash.
static std::string common_numeric(const std::string& a, const std::string& b) {
    if (a == b) return a;
    if (is_float_type(a)) return is_float_type(b) ? (float_bits(a) >= float_bits(b) ? a : b) : a;
    if (is_float_type(b)) return b;
    if (is_int_type(a) && is_int_type(b)) {
        if (is_signed_int(a) != is_signed_int(b)) return ""; // mixed signedness
        return int_bits(a) >= int_bits(b) ? a : b;
    }
    return "";
}

// True when the value of `src` may be implicitly widened into `dest`.
// Narrowing is never implicit; only compile-time constants may shrink (see
// constant_fits / assignable_expr).
static bool numeric_convertible(const std::string& src, const std::string& dest) {
    if (!is_numeric(src) || !is_numeric(dest)) return false;
    if (is_float_type(src) && is_float_type(dest))
        return float_bits(dest) >= float_bits(src);
    if (is_int_type(src) && is_float_type(dest)) return true; // any int -> any float
    if (is_int_type(src) && is_int_type(dest)) {
        if (is_signed_int(src) != is_signed_int(dest)) return false;
        return int_bits(dest) >= int_bits(src);
    }
    return false; // float -> int is never implicit
}

// True when the integer constant `v` (sign `negated`) fits in `dest`.
static bool int_const_fits(const std::string& dest, unsigned long long v, bool negated) {
    if (!is_int_type(dest)) return true;
    const int bits = int_bits(dest);
    if (is_unsigned_int(dest)) {
        if (negated) return false;
        return bits == 64 || v <= ((1ULL << bits) - 1);
    }
    // Signed: magnitude up to 2^(bits-1) (i32 min), or 2^(bits-1)-1 when
    // positive (i32 max).
    const unsigned long long limit = (bits == 64)
        ? (negated ? 0x8000000000000000ULL : 0x7FFFFFFFFFFFFFFFULL)
        : ((unsigned long long)1 << (bits - 1)) - (negated ? 0 : 1);
    return v <= limit;
}

// True when `e` is a numeric literal (Number or Neg(Number)); returns its
// magnitude and sign. Used for constant-range checks where inference types
// would otherwise force a widening (int) classification on the literal.
static bool unbox_int_literal(Expr* e, unsigned long long& v, bool& negated) {
    if (auto* num = dynamic_cast<Number*>(e)) {
        v = num->value; negated = false;
        return true;
    }
    if (auto* neg = dynamic_cast<Neg*>(e)) {
        if (auto* num = dynamic_cast<Number*>(neg->value.get())) {
            v = num->value; negated = true;
            return true;
        }
    }
    return false;
}

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
    // Enum name -> variant names (program order).
    std::map<std::string, std::vector<std::string>> enums_;

    // Scope stack. Top-level statements live at depth 0; every block,
    // function body, epoch, loop and if enters a new scope.
    std::vector<std::map<std::string, std::string>> scopes_;
    int epoch_depth_ = 0;
    int scope_depth_ = 0; // actual nesting level (function/block depth)
    // Variable -> epoch depth at which it was declared (-1 = no epoch).
    std::map<std::string, int> epoch_of_;
    // Variable -> scope depth at which it was declared.
    std::map<std::string, int> scope_depth_of_;

    const std::string* current_fn_ = nullptr;

    static bool is_primitive(const std::string& t) {
        return t == "bool" || t == "string" || is_numeric(t);
    }

    // True when the type references arena-allocated memory (slices, arrays,
    // strings, or structs containing such fields). Register-only types
    // (int, float, bool, enum) are safe to return from epochs.
    bool is_arena_linked_type(const std::string& t) {
        if (is_slice(t) || t == "string") return true;
        if (is_fixed_array(t)) return true;
        auto it = structs_.find(t);
        if (it != structs_.end()) {
            for (const auto& f : it->second.fields)
                if (is_arena_linked_type(f.var_type)) return true;
        }
        return false;
    }

    // True when the expression may reference arena memory.
    bool is_arena_linked_expr(Expr* e) {
        if (auto* n = dynamic_cast<New*>(e)) return true;
        if (auto* al = dynamic_cast<ArrayLit*>(e)) return true;
        if (auto* v = dynamic_cast<Variable*>(e)) {
            std::string t = lookup_var(v->name, e->line, e->col);
            return is_arena_linked_type(t);
        }
        if (auto* c = dynamic_cast<Call*>(e)) {
            auto fn = funcs_.find(c->callee);
            if (fn != funcs_.end())
                return is_arena_linked_type(fn->second.return_type);
            return false;
        }
        if (auto* ma = dynamic_cast<MemberAccess*>(e))
            return is_arena_linked_type(infer(ma));
        if (auto* ix = dynamic_cast<Index*>(e))
            return is_arena_linked_type(infer(ix));
        return false;
    }

    // Builtin explicit conversions (narrowing is only reachable through these).
    static bool is_conversion(const std::string& name) {
        return builtin_conversions().count(name) != 0;
    }

    // True when `variant` is declared by enum type `en`.
    bool is_enum_variant(const std::string& en, const std::string& variant) const;

    // True when `e` is a bare enum variant reference (e.g. `Color.red`).
    bool is_enum_variant_expr(Expr* e) const;

    static const std::set<std::string>& builtin_conversions() {
        static const std::set<std::string> s = {
            "to_int",  "to_float", "to_i8", "to_i16", "to_i32", "to_i64",
            "to_u8",   "to_u16",   "to_u32", "to_u64", "to_f32", "to_f64",
        };
        return s;
    }

    static const std::string conversion_target(const std::string& name) {
        if (name == "to_int") return "int";
        if (name == "to_float") return "float";
        return name.substr(3); // to_i64 -> i64
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

    // True when the compile-time constant `e` (a Number literal, possibly
    // negated or wrapped by a float conversion) fits in a slot of type `dest`.
    // On success returns true; otherwise reports a range diagnostic and returns
    // false. Non-constant expressions fall through to assignable().
    bool constant_fits(const std::string& dest, Expr* e);

    // True when `src` may be stored into a slot of type `dest`, using
    // constant-shrink for numeric literals and implicit widening otherwise.
    bool assignable(const std::string& dest, const std::string& src);

    // Assignability of an expression against a type: like assignable(), but
    // allows compile-time numeric constants to (re)fit any destination type.
    bool assignable_expr(const std::string& dest, Expr* e);

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
    int scope_depth_target(Expr* e);
    void check_escapes_target(Expr* target, Expr* value);

    void check_stmt(Stmt& s);
    void check_block(const std::vector<std::shared_ptr<Stmt>>& body);
    void check_epoch(Epoch& e);
};

// ---------------------------------------------------------------------------

bool Checker::assignable(const std::string& dest, const std::string& src) {
    if (src.empty()) return true; // the error is reported at the source
    if (dest == src) return true;
    // Numeric widening is the only implicit arithmetic conversion; shrinking
    // is explicit (to_* builtins) or resolved at constant time (assignable_expr).
    if (is_numeric(dest) && is_numeric(src)) return numeric_convertible(src, dest);
    // A slice may be written into a slice slot when element types agree
    // (array literals are coerced contextually, see infer()).
    if (is_slice(dest) && (is_slice(src) || is_fixed_array(src)))
        return element_type(dest) == element_type(src);
    return false;
}

// True when a checkable numeric constant fits `dest`. Returns false either for
// values that overflow (already reported) or for expressions constant_fits
// cannot adjudicate (non-literals) — callers must then fall back to
// assignable(infer()). An int literal always fits a float destination.
bool Checker::constant_fits(const std::string& dest, Expr* e) {
    unsigned long long v;
    bool negated;
    if (!unbox_int_literal(e, v, negated)) return false; // not a checkable constant
    if (is_float_type(dest)) return true;                // int literal -> any float
    if (!is_int_type(dest)) return false;                // e.g. literal into a struct/enum
    if (int_const_fits(dest, v, negated)) return true;
    const std::string msg = "integer literal out of range for " + dest;
    error(msg.c_str(), e->line, e->col);
    return false;
}

// True when `src` may be stored into a slot of type `dest`, using
// constant-shrink for numeric literals and implicit widening otherwise.
bool Checker::assignable_expr(const std::string& dest, Expr* e) {
    if (!e) return true;
    unsigned long long v;
    bool negated;
    if (constant_fits(dest, e)) return true;
    // An integer literal into an int slot either fit or was already diagnosed.
    if (is_int_type(dest) && unbox_int_literal(e, v, negated)) return false;
    if (auto* fl = dynamic_cast<Float*>(e))
        return is_float_type(dest);
    if (auto* al = dynamic_cast<ArrayLit*>(e)) {
        if (is_slice(dest) || is_fixed_array(dest)) {
            // Contextual coercion: the destination fixes the element type, so
            // each element must be assignable to it (constants may shrink).
            bool ok = true;
            for (const auto& el : al->elements)
                if (!assignable_expr(element_type(dest), el.get())) {
                    error("array literal element has an incompatible type",
                          el->line, el->col);
                    ok = false;
                }
            return ok;
        }
    }
    return assignable(dest, infer(e));
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
    scope_depth_of_[name] = scope_depth_;
}

bool Checker::is_enum_variant(const std::string& en, const std::string& variant) const {
    auto it = enums_.find(en);
    if (it == enums_.end()) return false;
    for (const auto& v : it->second)
        if (v == variant) return true;
    return false;
}

bool Checker::is_enum_variant_expr(Expr* e) const {
    auto* ma = dynamic_cast<MemberAccess*>(e);
    if (!ma) return false;
    auto* v = dynamic_cast<Variable*>(ma->object.get());
    if (!v || !enums_.count(v->name)) return false;
    return is_enum_variant(v->name, ma->member);
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
        // A bare literal over i32 max only fits when the negation targets a
        // widened slot, which assignable_expr/constant_fits decide downstream.
        if (auto* lit = dynamic_cast<Number*>(n->value.get()))
            if (lit->value > 0x80000000ULL)
                error("integer literal out of range for int (annotate a wider type)",
                      e->line, e->col);
        std::string t = infer(n->value.get());
        if (!is_signed_int(t) && !is_float_type(t))
            error("negation requires a signed number", e->line, e->col);
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
        std::string t1 = infer(c->left.get()), t2 = infer(c->right.get());
        if (!t1.empty() && !t2.empty()) {
            const bool relational = c->op == "<" || c->op == ">" ||
                                    c->op == "<=" || c->op == ">=";
            bool num_ok = false;
            if (is_numeric(t1) && is_numeric(t2)) {
                // Constants may shrink against the typed side's range.
                unsigned long long v; bool neg;
                num_ok = (unbox_int_literal(c->left.get(), v, neg)
                              ? (is_float_type(t2) || int_const_fits(t2, v, neg))
                              : true)
                      && (unbox_int_literal(c->right.get(), v, neg)
                              ? (is_float_type(t1) || int_const_fits(t1, v, neg))
                              : true)
                      && (!is_int_type(t1) || !is_int_type(t2) ||
                          common_numeric(t1, t2) != "" ||
                          unbox_int_literal(c->left.get(), v, neg) ||
                          unbox_int_literal(c->right.get(), v, neg));
            }
            if (relational) {
                if (!num_ok)
                    error("comparison requires numbers of compatible type", e->line, e->col);
            } else if (t1 != t2 && !num_ok) {
                error("== and != require operands of the same type", e->line, e->col);
            }
        }
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
        if (!is_int_type(idx)) error("array index must be an integer", e->line, e->col);
        return element_type(obj);
    }
    if (auto* ma = dynamic_cast<MemberAccess*>(e)) {
        // Enum variant: the object is the enum *type* name (`Color.red`),
        // not a variable. Detect it before the object-inference path.
        if (auto* tv = dynamic_cast<Variable*>(ma->object.get())) {
            auto eit = enums_.find(tv->name);
            if (eit != enums_.end()) {
                if (!is_enum_variant(tv->name, ma->member)) {
                    error("unknown enum variant", e->line, e->col);
                    return "";
                }
                return tv->name;
            }
        }
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
        if (!is_int_type(infer(n->size.get())))
            error("array size must be an integer", e->line, e->col);
        if (!is_primitive(n->base) && !structs_.count(n->base) && !enums_.count(n->base))
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
    if (lt.empty() || rt.empty()) return "";
    std::string res = common_numeric(lt, rt);
    if (res.empty()) {
        error("binary operator requires two numbers of compatible type", line, col);
        return "";
    }
    return res;
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

int Checker::scope_depth_target(Expr* e) {
    if (auto* v = dynamic_cast<Variable*>(e)) {
        auto it = scope_depth_of_.find(v->name);
        return it != scope_depth_of_.end() ? it->second : -1;
    }
    if (auto* ix = dynamic_cast<Index*>(e))
        return scope_depth_target(ix->object.get());
    if (auto* ma = dynamic_cast<MemberAccess*>(e))
        return scope_depth_target(ma->object.get());
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
        if (!assignable_expr(st.fields[i].var_type, sl->args[i].get()))
            error("struct literal field type mismatch", sl->line, sl->col);
    }
}

std::string Checker::check_call(Call* c) {
    // Explicit conversions are builtins: any numeric may be converted, and a
    // constant argument is range-checked against the target type.
    if (builtin_conversions().count(c->callee)) {
        if (c->args.size() != 1)
            error("conversion function expects one argument", c->line, c->col);
        else {
            std::string t = infer(c->args[0].get());
            if (!is_numeric(t))
                error("conversion requires a numeric argument", c->line, c->col);
            else
                constant_fits(conversion_target(c->callee), c->args[0].get());
        }
        return conversion_target(c->callee);
    }
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
            if (is_enum_variant_expr(c->args[i].get()))
                error("ref parameter cannot bind an enum variant", c->line, c->col);
        }
        if (!assignable_expr(p.var_type, c->args[i].get()))
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
    // Param escape check: if an arena-linked argument is passed to a parameter
    // that escapes, the value would dangle after the calling scope ends.
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (fn.escaping_params.count((int)i) && is_arena_linked_expr(c->args[i].get())) {
            int origin = arena_origin(c->args[i].get());
            if (origin > 0) {
                error("passing epoch-allocated value to function may cause escape",
                      c->line, c->col);
            }
        }
    }
    return fn.return_type;
}

void Checker::check_epoch(Epoch& e) {
    epoch_depth_++;
    scope_depth_++;
    scopes_.push_back({});
    for (auto& s : e.body) check_stmt(*s);
    scopes_.pop_back();
    scope_depth_--;
    epoch_depth_--;
}

void Checker::check_block(const std::vector<std::shared_ptr<Stmt>>& body) {
    scope_depth_++;
    scopes_.push_back({});
    for (auto& s : body) check_stmt(*s);
    scopes_.pop_back();
    scope_depth_--;
}

void Checker::check_stmt(Stmt& s) {
    switch (s.kind) {
    case Stmt::Kind::Let: {
        std::string t = s.let.var_type;
        if (t.empty()) {
            t = infer(s.let.value.get());
            // An untyped integer literal over i32 max cannot be stored as int.
            if (t == "int") {
                unsigned long long v; bool neg;
                if (unbox_int_literal(s.let.value.get(), v, neg))
                    constant_fits("int", s.let.value.get());
            }
        } else if (!assignable_expr(t, s.let.value.get())) {
            error("let initialiser type mismatch", s.line, s.col);
        }
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
        Expr* rv = s.return_stmt->value.get();
        auto it = funcs_.find(*current_fn_);
        if (it != funcs_.end() && !assignable_expr(it->second.return_type, rv))
            error("return value does not match function return type", s.line, s.col);
        if (epoch_depth_ > 0 && rv) {
            // Only arena-linked values (slices, arrays, strings, structs with
            // such fields) are forbidden. Primitives (int, float, bool) live
            // in registers and are safe to return.
            if (is_arena_linked_expr(rv)) {
                error("cannot return arena-allocated value from epoch", s.line, s.col);
            }
        }
        break;
    }
    case Stmt::Kind::Assign: {
        Assign* a = s.assign_stmt.get();
        std::string target_ty = infer(a->name.get());
        if (is_enum_variant_expr(a->name.get()))
            error("cannot assign to an enum variant", s.line, s.col);
        if (a->op != "=" && !is_numeric(target_ty))
            error("compound assignment requires a numeric target", s.line, s.col);
        if (!target_ty.empty() && !assignable_expr(target_ty, a->value.get()))
            error("cannot assign value of incompatible type", s.line, s.col);
        check_escapes_target(a->name.get(), a->value.get());
        // Track param escape: if value is a function parameter and target is
        // in an outer scope, mark the parameter as escaping.
        if (current_fn_ && a->op == "=") {
            if (auto* v = dynamic_cast<Variable*>(a->value.get())) {
                auto fn_it = funcs_.find(*current_fn_);
                if (fn_it != funcs_.end()) {
                    for (size_t i = 0; i < fn_it->second.params.size(); ++i) {
                        if (fn_it->second.params[i].name == v->name) {
                            int target_depth = scope_depth_target(a->name.get());
                            int param_depth = scope_depth_of_.count(v->name)
                                ? scope_depth_of_[v->name] : -1;
                            if (target_depth >= 0 && target_depth < param_depth) {
                                fn_it->second.escaping_params.insert((int)i);
                            }
                        }
                    }
                }
            }
        }
        break;
    }
    case Stmt::Kind::FuncDecl: {
        scope_depth_++;
        scopes_.push_back({});
        SemaFn& fn = funcs_[s.func_decl->name];
        for (const Param& p : fn.params) {
            declare_var(p.name, p.var_type, s.line, s.col);
        }
        current_fn_ = &s.func_decl->name;
        // Check body directly (not via check_block) to keep params at same scope depth.
        for (auto& s : s.func_decl->body->body) check_stmt(*s);
        current_fn_ = nullptr;
        scopes_.pop_back();
        scope_depth_--;
        break;
    }
    case Stmt::Kind::CallStmt:
        infer(s.call_stmt.get());
        break;
    case Stmt::Kind::StructDecl:
        break; // collected in pass 1
    case Stmt::Kind::EnumDecl:
        break; // collected in pass 1, which registers the enum variants
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
        case Stmt::Kind::EnumDecl: {
            if (!s->enum_decl) break;
            const EnumDecl* e = s->enum_decl.get();
            if (!enums_.emplace(e->name, e->variants).second) {
                error("redeclaration of enum", s->line, s->col);
                break;
            }
            std::set<std::string> seen;
            for (const std::string& v : e->variants)
                if (!seen.insert(v).second)
                    error("duplicate enum variant", s->line, s->col);
            break;
        }
        default:
            break;
        }
    }
    // Type/function/enum names share one namespace.
    for (const auto& e : enums_) {
        if (funcs_.count(e.first) || structs_.count(e.first)) {
            error("enum name conflicts with an existing function or struct", 0, 0);
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