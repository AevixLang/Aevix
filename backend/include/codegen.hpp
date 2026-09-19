#pragma once
#include "json_reader.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <set>

class CodeGenerator {
private:
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    llvm::Function* main_func;
    llvm::BasicBlock* main_entry_block;
    llvm::Function* oob_func;
    std::map<std::string, llvm::Function*> functions;
    std::vector<std::map<std::string, llvm::Value*>> named_values;
    std::vector<std::map<std::string, llvm::Type*>> named_types;
    // Semantic type names for variables (e.g. "int", "i64", "u8", "Color").
    // Mirrors named_values scope-for-scope; codegen uses this to decide
    // how to coerce values without relying solely on LLVM types.
    std::vector<std::map<std::string, std::string>> var_type_names_;
    // Per-scope flag: a name declared as a ref parameter reads and writes
    // through an extra pointer layer stored on the stack (an alloca of the
    // caller's address). Mirrors named_values scope-for-scope.
    std::vector<std::set<std::string>> ref_scopes_;
    // Per-function parameter table, used to know which call arguments are
    // passed by reference/out at call sites.
    std::map<std::string, std::vector<Param>> func_params_;
    std::vector<llvm::Type*> return_type_stack;
    // Semantic return type name per function (e.g. "int", "float", "Color[]").
    std::map<std::string, std::string> return_type_names_;
    // Matching stack for return_type_stack: the type *name* of the current
    // function's return type ("" for void). Drives coerce_expr_to on return.
    std::vector<std::string> return_name_stack;
    std::set<std::string> open_arrays;
    std::vector<std::string> open_array_order;
    std::map<std::string, llvm::Type*> open_array_elem_types;
    // Element type *name* of each open array (e.g. "i64", "Color", "string").
    std::map<std::string, std::string> open_array_elem_type_names;
    std::vector<std::size_t> scope_open_arrays_count;

    // Active loop context for break/continue: each entry is {break BB, continue BB}.
    // continue for a for() loop points at the step block; for while/for-in it is
    // the condition block. Empty when not inside a loop -> break/continue errors.
    std::vector<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>> loop_ctx;

    // Slice { ptr, len }: the single representation of open arrays (int[]).
    // Keyed by the base type string ("int", "float", "bool", struct name).
    std::map<std::string, llvm::StructType*> slice_types;
    std::map<llvm::StructType*, std::string> slice_bases;

    // Arena runtime helpers
    llvm::Function* alloc_func;
    llvm::Function* epoch_begin_func;
    llvm::Function* epoch_end_func;

    // I/O runtime helpers: program arguments, exit code, file read/write.
    llvm::GlobalVariable* argc_global;
    llvm::GlobalVariable* argv_global;
    llvm::Function* strlen_func;
    llvm::Function* exit_func;
    llvm::Function* read_file_func;
    llvm::Function* write_file_func;
    llvm::Function* read_line_func;
    void build_io_runtime();

    // String conversion runtime helpers (aevix_runtime.c): parsing, formatting
    // and slicing. to_str has two backends for int vs float arguments.
    llvm::Function* to_int_func;
    llvm::Function* to_float_func;
    llvm::Function* int_to_str_func;
    llvm::Function* double_to_str_func;
    llvm::Function* i64_to_str_func;
    llvm::Function* u64_to_str_func;
    llvm::Function* substr_func;
    llvm::Function* split_func;
    void build_conv_runtime();

    // Escape-checking for epochs: every open-array variable remembers the
    // epoch depth it was created at. Assigning a slice to a variable defined
    // at a smaller depth would survive the arena rollback and dangle.
    int epoch_depth = 0;
    std::map<std::string, int> open_epochs;

    // Struct handling
    std::map<std::string, const StructDecl*> struct_decls;
    std::map<std::string, llvm::StructType*> struct_types;
    std::map<std::string, std::map<std::string, int>> struct_field_indices;
    // Struct field type names (e.g. "Pixel.color" -> "Color").
    std::map<std::string, std::map<std::string, std::string>> field_type_names_;

    // Enum values are represented as i32 indices (CNAME order in the enum type).
    std::set<std::string> enum_names_;
    std::map<std::string, std::vector<std::string>> enum_variants_;
    std::map<std::string, int> enum_variant_indices_; // "Color.red" -> 0

    // Source position of the current expression/statement, used by error().
    int current_line = 0;
    int current_col = 0;

    void push_scope();
    void pop_scope();
    void register_open_array(const std::string& name, llvm::Type* elem_ty);
    void build_oob_runtime();
    void build_arena_runtime();
    void define_var(const std::string& name, llvm::Value* alloc, llvm::Type* ty);
    // define_var with a semantic type name (e.g. "i64", "Color"); the type name
    // drives coercion decisions when the LLVM type alone is ambiguous.
    void define_var(const std::string& name, llvm::Value* alloc, llvm::Type* ty, const std::string& type_name);
    std::string lookup_var_type_name(const std::string& name);
    llvm::Value* lookup_var(const std::string& name, llvm::Type*& ty);
    llvm::Value* lookup_var_indexed(const std::string& name, llvm::Type*& ty, std::size_t& idx);
    bool is_ref_var(const std::string& name);
    // Resolves the address of an lvalue passed to a ref parameter
    // (dereferencing one pointer layer when the lvalue is itself a ref
    // parameter of the current function).
    llvm::Value* gen_ref_arg_ptr(const std::shared_ptr<Expr>& e);
    llvm::Type* llvm_type_for(const std::string& tn);
    bool parse_type(const std::string& tn, std::string& base, int& arr_size);
    llvm::Type* scalar_type_for(const std::string& base);
    llvm::Type* signature_type_for(const std::string& tn, const std::string& ctx);
    [[noreturn]] void error(const std::string& msg);

    // Slice helpers
    llvm::StructType* slice_type_for(const std::string& base);
    llvm::Type* slice_elem_type(llvm::StructType* st);
    bool is_slice_ty(llvm::Type* ty);
    llvm::Value* make_slice(llvm::Value* ptr, const std::string& base, llvm::Type* elem, llvm::Value* len);
    llvm::Value* copy_array_to_slice(llvm::Value* arr_val, llvm::StructType* slice_ty);
    llvm::Value* coerce_to_slice(llvm::Value* val, llvm::StructType* slice_ty, const std::string& ctx);
    void emit_slice_print(llvm::Value* slice);
    void emit_slice_print(llvm::Value* slice, bool trailing_newline);
    // Prints a scalar value with the printf format matching `aevix_name`
    // (i8/u8→char, i16/u16/u32/i64/u64→number, f32→f64, bool→true/false,
    // enum→variant name).
    void emit_print_scalar(llvm::Value* val, const std::string& aevix_name);
    // Prints a loaded struct value as "{ f1, f2 ... }".
    void emit_print_struct_inline(llvm::Value* val, const std::string& type_name);
    llvm::Value* gen_new(const New& n);
    void generate_epoch(const Epoch& ep);

    // Strings are slices of i8: { i8*, i32 }. build the value or test a type.
    llvm::Value* make_string_slice(llvm::Value* ptr, llvm::Value* len);
    // string[] is a slice whose elements are string slices: { string*, i32 }.
    llvm::Value* make_string_array_slice(llvm::Value* ptr, llvm::Value* len);
    bool is_string_slice(llvm::Type* ty);
    llvm::Value* gen_string_concat(llvm::Value* left, llvm::Value* right);
    llvm::Function* str_eq_func;

    // Type-checking helpers: validate that a generated value is a numeric type
    // (int/float) or a bool before an operation that requires it.
    void promote_binop_operands(llvm::Value*& left, llvm::Value*& right);
    // Promotes both operands to their common numeric type name (sema's
    // rule: same-signedness widening only; clashing kinds are a sema error).
    void promote_binop_operands(llvm::Value*& left, llvm::Value*& right,
                                const std::string& lname, const std::string& rname);
    bool is_numeric(llvm::Type* ty);
    void require_numeric(llvm::Value* v, const std::string& ctx);
    void require_bool(llvm::Value* v, const std::string& ctx);
    void require_bool_type(llvm::Type* ty, const std::string& ctx);
    std::string llvm_type_name(llvm::Type* ty);

    // Fixed-width type classification (mirrors sema): type-name level helpers.
    bool is_int_type_name(const std::string& tn);
    bool is_float_type_name(const std::string& tn);
    bool is_numeric_type_name(const std::string& tn);
    bool type_is_unsigned(const std::string& tn);
    // True when sema would allow an implicit (widening-only) conversion.
    bool can_implicit_coerce(const std::string& src, const std::string& dest);
    int type_width_bits(const std::string& tn);
    int float_width_bits(const std::string& tn);
    std::string common_numeric_name(const std::string& a, const std::string& b);
    // Maps an LLVM type back to its canonical Aevix type name ("int", "i64",
    // "f32", "bool", "string", "Pixel", "Pixel[]", "int[3]", ...).
    std::string lltype_to_name(llvm::Type* ty);
    // True when the named type is a slice destination ("int[]", "string").
    bool is_slice_dest(const std::string& tn);

    // Enum helpers
    bool is_enum(const std::string& name);
    bool is_enum_variant(const std::string& en, const std::string& variant);
    int enum_variant_index(const std::string& en, const std::string& variant);
    void register_enum(const EnumDecl& ed);
    // True when `e` is a Color.red style enum variant expression.
    bool is_enum_variant_expr(const std::shared_ptr<Expr>& e);
    // Emits/copies a value into the LLVM type for `dest` with the semantics of
    // a numeric conversion (both already validated by sema).
    llvm::Value* coerce_numeric(llvm::Value* v, const std::string& src, const std::string& dest);
    // Coerce an expression to a named target type ("int", "i64", "float",
    // "Color", "int[3]", "int[]", ...). Rebuilds array literals and
    // materializes integer literals at the target width.
    llvm::Value* coerce_expr_to(const std::shared_ptr<Expr>& e, const std::string& dest);
    // Builds a slice of `base` from an array literal, coercing each element.
    llvm::Value* make_slice_from_lit(const ArrayLit& al, const std::string& base);
    // Semantic type name of an expression (e.g. "int", "i64", "Color", ...
    // arrays as "int[3]", slices as "int[]"). Used to coerce values.
    std::string expr_type_name(const std::shared_ptr<Expr>& e);
    std::string fn_type_name(const std::string& tn);

    // Open array helpers
    bool is_open_array(const std::string& name);
    llvm::Type* open_array_elem_type(const std::string& name);

    // Struct helpers
    bool is_struct(const std::string& tn);
    llvm::StructType* struct_type_for(const std::string& tn);
    int struct_field_index(const std::string& st, const std::string& field);
    llvm::Type* struct_field_type(const std::string& st, const std::string& field);
    void register_struct(const StructDecl& sd);
    llvm::Type* llvm_type_from_name(const std::string& tn);
    llvm::Value* gen_member_ptr(const MemberAccess& ma);
    llvm::Value* gen_member_ptr_inner(const MemberAccess& ma, llvm::Type*& field_ty);
    llvm::Value* build_struct_literal(const StructLiteral& sl);

    // Array helpers: element type inference, literal -> constant, and lvalue
    // resolution through (possibly nested) index expressions.
    llvm::Type* element_type_of(const std::shared_ptr<Expr>& e);
    llvm::Type* build_array_type(const ArrayLit& al);
    llvm::Constant* build_array_constant(const ArrayLit& al);
    // Folds the element type *name* of an array literal across all elements
    // (e.g. "int", "i64", "string", "Color"); errors on incompatible kinds.
    std::string array_elem_type_name(const ArrayLit& al);
    llvm::Value* gen_index_ptr(const Index& ix, llvm::Type*& elem_ty);

public:
    CodeGenerator();

    void generate_program(const Program& program);
    llvm::Value* generate_expr(const std::shared_ptr<Expr>& expr);
    llvm::Value* generate_icmp(const std::string& op, llvm::Value* left, llvm::Value* right);
    llvm::Value* generate_icmp(const std::string& op, llvm::Value* left, llvm::Value* right, bool is_unsigned);
    llvm::Value* generate_fcmp(const std::string& op, llvm::Value* left, llvm::Value* right);
    void generate_stmt(const std::shared_ptr<Stmt>& stmt);
    void generate_let(const Let& let);
    void generate_hot(const Hot& hot);
    void generate_print(const Print& print);
    void generate_block(const std::shared_ptr<Block>& block);
    void generate_if(const If& if_stmt);
    void generate_while(const While& w);
    void generate_for(const For& f);
    void generate_for_in(const ForIn& f);
    void generate_break(const Break& b);
    void generate_continue(const Continue& c);
    void generate_return(const Return& r);
    void generate_assign(const Assign& a);
    llvm::Value* apply_compound(llvm::Value* old, llvm::Value* rhs, const std::string& op,
                                llvm::Type* target_ty, const std::string& target_name,
                                const std::string& rhs_name, const std::string& ctx,
                                bool is_unsigned);
    void emit_print_value(const std::shared_ptr<Expr>& value);
    void declare_func(const FuncDecl& fd);
    void generate_func_decl(const FuncDecl& fd);
    llvm::Value* generate_logical_and(const And& and_);
    llvm::Value* generate_logical_or(const Or& or_);
    bool block_has_terminator(llvm::BasicBlock* bb);
    void finalize();
};