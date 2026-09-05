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
    std::vector<llvm::Type*> return_type_stack;
    std::set<std::string> open_arrays;
    std::vector<std::string> open_array_order;
    std::map<std::string, llvm::Type*> open_array_elem_types;
    std::vector<std::size_t> scope_open_arrays_count;

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
    llvm::Function* substr_func;
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

    // Source position of the current expression/statement, used by error().
    int current_line = 0;
    int current_col = 0;

    void push_scope();
    void pop_scope();
    void register_open_array(const std::string& name, llvm::Type* elem_ty);
    void build_oob_runtime();
    void build_arena_runtime();
    void define_var(const std::string& name, llvm::Value* alloc, llvm::Type* ty);
    llvm::Value* lookup_var(const std::string& name, llvm::Type*& ty);
    llvm::Type* llvm_type_for(const std::string& tn);
    bool parse_type(const std::string& tn, std::string& base, int& arr_size);
    llvm::Type* scalar_type_for(const std::string& base);
    llvm::Type* signature_type_for(const std::string& tn, const std::string& ctx);
    [[noreturn]] void error(const std::string& msg);

    // Slice helpers
    llvm::StructType* slice_type_for(const std::string& base);
    llvm::Type* slice_elem_type(llvm::StructType* st);
    bool is_slice_ty(llvm::Type* ty);
    llvm::Value* make_slice(llvm::Value* ptr, llvm::Type* elem, llvm::Value* len);
    llvm::Value* copy_array_to_slice(llvm::Value* arr_val, llvm::StructType* slice_ty);
    llvm::Value* coerce_to_slice(llvm::Value* val, llvm::StructType* slice_ty, const std::string& ctx);
    void emit_slice_print(llvm::Value* slice);
    void emit_slice_print(llvm::Value* slice, bool trailing_newline);
    llvm::Value* gen_new(const New& n);
    void generate_epoch(const Epoch& ep);

    // Strings are slices of i8: { i8*, i32 }. build the value or test a type.
    llvm::Value* make_string_slice(llvm::Value* ptr, llvm::Value* len);
    bool is_string_slice(llvm::Type* ty);
    llvm::Value* gen_string_concat(llvm::Value* left, llvm::Value* right);
    llvm::Function* str_eq_func;

    // Type-checking helpers: validate that a generated value is a numeric type
    // (int/float) or a bool before an operation that requires it.
    void promote_binop_operands(llvm::Value*& left, llvm::Value*& right);
    bool is_numeric(llvm::Type* ty);
    void require_numeric(llvm::Value* v, const std::string& ctx);
    void require_bool(llvm::Value* v, const std::string& ctx);
    void require_bool_type(llvm::Type* ty, const std::string& ctx);
    std::string llvm_type_name(llvm::Type* ty);

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
    llvm::Value* gen_index_ptr(const Index& ix, llvm::Type*& elem_ty);

public:
    CodeGenerator();

    void generate_program(const Program& program);
    llvm::Value* generate_expr(const std::shared_ptr<Expr>& expr);
    llvm::Value* generate_icmp(const std::string& op, llvm::Value* left, llvm::Value* right);
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
    void generate_return(const Return& r);
    void generate_assign(const Assign& a);
    void declare_func(const FuncDecl& fd);
    void generate_func_decl(const FuncDecl& fd);
    llvm::Value* generate_logical_and(const And& and_);
    llvm::Value* generate_logical_or(const Or& or_);
    bool block_has_terminator(llvm::BasicBlock* bb);
    void finalize();
};