// ============================================================================
// Aevix Backend: LLVM IR Code Generation
//
// This module handles the transformation of the Aevix AST into LLVM Intermediate
// Representation (IR). It manages variable scoping, type checking, and emits
// the final machine-executable code.
// ============================================================================
#include "codegen.hpp"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/TargetSelect.h"
#include <iostream>
#include <map>
#include <stack>

// ============================================================================
// Lifecycle: Module Setup & Variable Scope Management
// ============================================================================

CodeGenerator::CodeGenerator()
    : context(std::make_unique<llvm::LLVMContext>())
    , module(std::make_unique<llvm::Module>("aevix", *context))
    , builder(std::make_unique<llvm::IRBuilder<>>(*context))
{
    module->setDataLayout("e-m:e-p:64:64-i64:64-n32:64-S128");
    auto ptr_ty = llvm::PointerType::getUnqual(*context);
    auto func_type = llvm::FunctionType::get(builder->getInt32Ty(), {builder->getInt32Ty(), ptr_ty->getPointerTo()}, false);
    main_func = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage, "main", module.get());
    main_func->getArg(0)->setName("argc");
    main_func->getArg(1)->setName("argv");
    main_entry_block = llvm::BasicBlock::Create(*context, "entry", main_func);
    builder->SetInsertPoint(main_entry_block);

    build_oob_runtime();
    build_arena_runtime();
    build_io_runtime();
    build_conv_runtime();

    named_values.emplace_back();
    named_types.emplace_back();
    var_type_names_.emplace_back();
    ref_scopes_.emplace_back();
}

/**
 * Builds the runtime array bounds-check helper.
 * Prints an error message and exits(1) on out-of-bounds access.
 */
void CodeGenerator::build_oob_runtime() {
    auto i32 = builder->getInt32Ty();
    auto void_ty = llvm::Type::getVoidTy(*context);
    auto ptr_ty = llvm::PointerType::getUnqual(*context);

    llvm::FunctionType* ft = llvm::FunctionType::get(void_ty, {i32, i32}, false);
    oob_func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "__aevix_oob", module.get());

    auto body = llvm::BasicBlock::Create(*context, "entry", oob_func);
    llvm::IRBuilder<> tmp(*context);
    tmp.SetInsertPoint(body);

    auto fmt = tmp.CreateGlobalString("array index %d out of bounds (size %d)\n", "oob_fmt");

    llvm::FunctionType* printf_ft = llvm::FunctionType::get(i32, {ptr_ty}, true);
    module->getOrInsertFunction("printf", printf_ft);
    auto printf_fn = module->getFunction("printf");
    tmp.CreateCall(printf_fn, {fmt, oob_func->arg_begin(), oob_func->arg_begin() + 1});

    llvm::FunctionType* exit_ft = llvm::FunctionType::get(void_ty, {i32}, false);
    module->getOrInsertFunction("exit", exit_ft);
    auto exit_fn = module->getFunction("exit");
    exit_fn->addFnAttr(llvm::Attribute::NoReturn);
    tmp.CreateCall(exit_fn, {tmp.getInt32(1)});
    tmp.CreateUnreachable();
}

/**
 * Declares the arena (bump allocator) runtime, defined in aevix_runtime.c.
 * __aevix_alloc(bytes) hands out memory in O(1) by bumping a chunked virtual
 * mapping forward; memory is never freed individually. Epochs save/restore the
 * bump state, which logically rolls back every allocation made in between.
 */
void CodeGenerator::build_arena_runtime() {
    auto i32 = builder->getInt32Ty();
    auto void_ty = llvm::Type::getVoidTy(*context);
    auto ptr_ty = llvm::PointerType::getUnqual(*context);

    // __aevix_alloc(i32 bytes) -> ptr  (returns 16-byte aligned memory)
    auto alloc_ft = llvm::FunctionType::get(ptr_ty, {i32}, false);
    alloc_func = llvm::Function::Create(alloc_ft, llvm::Function::ExternalLinkage, "__aevix_alloc", module.get());

    // __aevix_epoch_begin() -> i32  (returns a frame token)
    auto begin_ft = llvm::FunctionType::get(i32, {}, false);
    epoch_begin_func = llvm::Function::Create(begin_ft, llvm::Function::ExternalLinkage, "__aevix_epoch_begin", module.get());

    // __aevix_epoch_end(i32 frame)  (restores the saved chunk and offset)
    auto end_ft = llvm::FunctionType::get(void_ty, {i32}, false);
    epoch_end_func = llvm::Function::Create(end_ft, llvm::Function::ExternalLinkage, "__aevix_epoch_end", module.get());

    // __aevix_str_eq(ptr a, i32 alen, ptr b, i32 blen) -> i1  (content compare)
    auto str_eq_ft = llvm::FunctionType::get(builder->getInt1Ty(), {ptr_ty, i32, ptr_ty, i32}, false);
    str_eq_func = llvm::Function::Create(str_eq_ft, llvm::Function::ExternalLinkage, "__aevix_str_eq", module.get());
}

/**
 * Declares the I/O runtime: program arguments, exit code and file read/write,
 * backed by libc. Read/write live in aevix_runtime.c; the rest are stdlib.
 */
void CodeGenerator::build_io_runtime() {
    auto i32 = builder->getInt32Ty();
    auto void_ty = llvm::Type::getVoidTy(*context);
    auto ptr_ty = llvm::PointerType::getUnqual(*context);

    // Args for the Aevix program: copies of argc/argv live in globals so any
    // function (not just hot) can call argc() and arg(i).
    argc_global = new llvm::GlobalVariable(*module, i32, false,
        llvm::GlobalValue::InternalLinkage, builder->getInt32(0), "__aevix_argc");
    argv_global = new llvm::GlobalVariable(*module, ptr_ty->getPointerTo(), false,
        llvm::GlobalValue::InternalLinkage, llvm::ConstantPointerNull::get(ptr_ty->getPointerTo()), "__aevix_argv");

    // strlen(ptr) -> i32
    auto strlen_ft = llvm::FunctionType::get(i32, {ptr_ty}, false);
    strlen_func = llvm::Function::Create(strlen_ft, llvm::Function::ExternalLinkage, "strlen", module.get());

    // exit(i32) (noreturn) — reuse the declaration made by build_oob_runtime.
    auto exit_ft = llvm::FunctionType::get(void_ty, {i32}, false);
    module->getOrInsertFunction("exit", exit_ft);
    exit_func = module->getFunction("exit");
    exit_func->addFnAttr(llvm::Attribute::NoReturn);

    // __aevix_read_file(ptr path, ptr* out_ptr, ptr* out_len)
    // Fills out_ptr/out_len with arena-backed file contents; empty on failure.
    auto pp_ty = llvm::PointerType::getUnqual(ptr_ty);
    auto read_ft = llvm::FunctionType::get(void_ty, {ptr_ty, pp_ty, pp_ty}, false);
    read_file_func = llvm::Function::Create(read_ft, llvm::Function::ExternalLinkage, "__aevix_read_file", module.get());

    // __aevix_write_file(ptr path, ptr data, i32 len) -> i32 (1 = ok)
    auto write_ft = llvm::FunctionType::get(i32, {ptr_ty, ptr_ty, i32}, false);
    write_file_func = llvm::Function::Create(write_ft, llvm::Function::ExternalLinkage, "__aevix_write_file", module.get());

    // __aevix_read_line(ptr* out_ptr, ptr* out_len) — one line from stdin; "" on EOF.
    auto line_ft = llvm::FunctionType::get(void_ty, {pp_ty, pp_ty}, false);
    read_line_func = llvm::Function::Create(line_ft, llvm::Function::ExternalLinkage, "__aevix_read_line", module.get());
}

/**
 * Declares the string conversion runtime, defined in aevix_runtime.c: parsing a
 * slice into a number, formatting numbers into arena strings, and substr. All
 * string-producing helpers write into (ptr* out_ptr, i32* out_len) like read().
 */
void CodeGenerator::build_conv_runtime() {
    auto i32 = builder->getInt32Ty();
    auto void_ty = llvm::Type::getVoidTy(*context);
    auto ptr_ty = llvm::PointerType::getUnqual(*context);
    auto pp_ty = llvm::PointerType::getUnqual(ptr_ty);

    // __aevix_parse_int(ptr s, i32 len) -> i32
    auto int_ft = llvm::FunctionType::get(i32, {ptr_ty, i32}, false);
    to_int_func = llvm::Function::Create(int_ft, llvm::Function::ExternalLinkage, "__aevix_parse_int", module.get());

    // __aevix_parse_double(ptr s, i32 len) -> double
    auto dbl_ty = builder->getDoubleTy();
    auto dbl_ft = llvm::FunctionType::get(dbl_ty, {ptr_ty, i32}, false);
    to_float_func = llvm::Function::Create(dbl_ft, llvm::Function::ExternalLinkage, "__aevix_parse_double", module.get());

    // __aevix_int_to_str(i32 n, ptr* out_ptr, i32* out_len)
    auto str_out_ft = llvm::FunctionType::get(void_ty, {i32, pp_ty, pp_ty}, false);
    int_to_str_func = llvm::Function::Create(str_out_ft, llvm::Function::ExternalLinkage, "__aevix_int_to_str", module.get());

    // __aevix_double_to_str(double d, ptr* out_ptr, i32* out_len)
    auto dstr_out_ft = llvm::FunctionType::get(void_ty, {dbl_ty, pp_ty, pp_ty}, false);
    double_to_str_func = llvm::Function::Create(dstr_out_ft, llvm::Function::ExternalLinkage, "__aevix_double_to_str", module.get());

    // __aevix_i64_to_str(i64 n, ptr* out_ptr, i32* out_len)
    // __aevix_u64_to_str(i64 n, ptr* out_ptr, i32* out_len)
    auto i64_ty = builder->getInt64Ty();
    auto i64_str_ft = llvm::FunctionType::get(void_ty, {i64_ty, pp_ty, pp_ty}, false);
    i64_to_str_func = llvm::Function::Create(i64_str_ft, llvm::Function::ExternalLinkage, "__aevix_i64_to_str", module.get());
    u64_to_str_func = llvm::Function::Create(i64_str_ft, llvm::Function::ExternalLinkage, "__aevix_u64_to_str", module.get());

    // __aevix_substr(ptr s, i32 len, i32 start, i32 count, ptr* out_ptr, i32* out_len)
    auto sub_ft = llvm::FunctionType::get(void_ty, {ptr_ty, i32, i32, i32, pp_ty, pp_ty}, false);
    substr_func = llvm::Function::Create(sub_ft, llvm::Function::ExternalLinkage, "__aevix_substr", module.get());

    // __aevix_split(ptr s, i32 slen, ptr sep, i32 seplen, ptr* out_ptr, i32* out_len)
    // out_ptr receives a string[] slice: { string*, i32 } in arena memory.
    auto split_ft = llvm::FunctionType::get(void_ty, {ptr_ty, i32, ptr_ty, i32, pp_ty, pp_ty}, false);
    split_func = llvm::Function::Create(split_ft, llvm::Function::ExternalLinkage, "__aevix_split", module.get());
}

/**
 * Throws a runtime error to stop IR emission upon discovering a semantic fault.
 */
void CodeGenerator::error(const std::string& msg) {
    if (current_line > 0) {
        throw std::runtime_error(msg + " at line " + std::to_string(current_line) +
                                 ", column " + std::to_string(current_col));
    }
    throw std::runtime_error(msg);
}

void CodeGenerator::push_scope() {
    named_values.emplace_back();
    named_types.emplace_back();
    var_type_names_.emplace_back();
    ref_scopes_.emplace_back();
    scope_open_arrays_count.push_back(open_arrays.size());
}

void CodeGenerator::pop_scope() {
    named_values.pop_back();
    named_types.pop_back();
    var_type_names_.pop_back();
    ref_scopes_.pop_back();
    if (!scope_open_arrays_count.empty()) {
        auto target = scope_open_arrays_count.back();
        scope_open_arrays_count.pop_back();
        while (open_array_order.size() > target) {
            std::string name = open_array_order.back();
            open_array_order.pop_back();
            open_arrays.erase(name);
            open_epochs.erase(name);
            open_array_elem_types.erase(name);
        }
    }
}

void CodeGenerator::register_open_array(const std::string& name, llvm::Type* elem_ty) {
    open_arrays.insert(name);
    open_array_order.push_back(name);
    open_array_elem_types[name] = elem_ty;
}

bool CodeGenerator::is_open_array(const std::string& name) {
    return open_arrays.find(name) != open_arrays.end();
}

llvm::Type* CodeGenerator::open_array_elem_type(const std::string& name) {
    auto it = open_array_elem_types.find(name);
    if (it != open_array_elem_types.end()) return it->second;
    return builder->getInt32Ty();
}

// ============================================================================
// Slice Handling: open arrays (int[]) as a single { base*, i32 } value
// ============================================================================

llvm::StructType* CodeGenerator::slice_type_for(const std::string& base) {
    auto it = slice_types.find(base);
    if (it != slice_types.end()) return it->second;
    std::string sname = base + "Slice";
    if (base == "string[]") sname = "stringArraySlice";
    auto st = llvm::StructType::create(
        *context,
        {llvm::PointerType::getUnqual(*context), builder->getInt32Ty()},
        sname);
    slice_types[base] = st;
    slice_bases[st] = base;
    return st;
}

llvm::Type* CodeGenerator::slice_elem_type(llvm::StructType* st) {
    auto it = slice_bases.find(st);
    if (it == slice_bases.end()) return nullptr;
    if (it->second == "string[]") return slice_type_for("string");
    llvm::Type* e = scalar_type_for(it->second);
    if (e) return e;
    auto sit = struct_types.find(it->second);
    if (sit != struct_types.end()) return sit->second;
    return nullptr;
}

bool CodeGenerator::is_slice_ty(llvm::Type* ty) {
    if (!ty->isStructTy()) return false;
    return slice_bases.count(llvm::cast<llvm::StructType>(ty)) > 0;
}

llvm::Value* CodeGenerator::make_slice(llvm::Value* ptr, const std::string& base, llvm::Type* elem, llvm::Value* len) {
    llvm::StructType* st = slice_type_for(base);
    auto slice = builder->CreateInsertValue(llvm::UndefValue::get(st), ptr, 0, "slice");
    slice = builder->CreateInsertValue(slice, len, 1, "slice.len");
    return slice;
}

llvm::Value* CodeGenerator::make_string_slice(llvm::Value* ptr, llvm::Value* len) {
    llvm::StructType* st = slice_type_for("string");
    auto slice = builder->CreateInsertValue(llvm::UndefValue::get(st), ptr, 0, "str");
    slice = builder->CreateInsertValue(slice, len, 1, "str.len");
    return slice;
}

llvm::Value* CodeGenerator::make_string_array_slice(llvm::Value* ptr, llvm::Value* len) {
    llvm::StructType* st = slice_type_for("string[]");
    auto slice = builder->CreateInsertValue(llvm::UndefValue::get(st), ptr, 0, "sarr");
    slice = builder->CreateInsertValue(slice, len, 1, "sarr.len");
    return slice;
}

bool CodeGenerator::is_string_slice(llvm::Type* ty) {
    if (!ty || !ty->isStructTy()) return false;
    auto it = slice_bases.find(llvm::cast<llvm::StructType>(ty));
    return it != slice_bases.end() && it->second == "string";
}

llvm::Value* CodeGenerator::gen_string_concat(llvm::Value* left, llvm::Value* right) {
    auto lp = builder->CreateExtractValue(left, 0, "cat.lp");
    auto llen = builder->CreateExtractValue(left, 1, "cat.llen");
    auto rp = builder->CreateExtractValue(right, 0, "cat.rp");
    auto rlen = builder->CreateExtractValue(right, 1, "cat.rlen");

    auto total32 = builder->CreateAdd(llen, rlen, "cat.total");
    auto bytes32 = builder->CreateTrunc(builder->CreateZExt(total32, builder->getInt64Ty()), builder->getInt32Ty(), "cat.bytes");
    auto dst = builder->CreateCall(alloc_func, {bytes32}, "cat.ptr");

    llvm::Function* memcpy_fn = llvm::Intrinsic::getOrInsertDeclaration(
        module.get(), llvm::Intrinsic::memcpy, llvm::Type::getVoidTy(*context),
        {llvm::PointerType::getUnqual(*context), llvm::PointerType::getUnqual(*context),
         builder->getInt64Ty(), builder->getInt1Ty()});
    builder->CreateCall(memcpy_fn, {dst, lp, builder->CreateZExt(llen, builder->getInt64Ty(), "cat.l64"), builder->getInt1(false)});
    auto dstr = builder->CreateInBoundsGEP(builder->getInt8Ty(), dst, llen, "cat.mid");
    builder->CreateCall(memcpy_fn, {dstr, rp, builder->CreateZExt(rlen, builder->getInt64Ty(), "cat.r64"), builder->getInt1(false)});

    return make_string_slice(dst, total32);
}

void CodeGenerator::define_var(const std::string& name, llvm::Value* alloc, llvm::Type* ty) {
    define_var(name, alloc, ty, "");
}

void CodeGenerator::define_var(const std::string& name, llvm::Value* alloc, llvm::Type* ty, const std::string& type_name) {
    named_values.back()[name] = alloc;
    named_types.back()[name] = ty;
    var_type_names_.back()[name] = type_name;
}

// Semantic type name of a variable ("" when unknown). Scans scopes outermost-in.
std::string CodeGenerator::lookup_var_type_name(const std::string& name) {
    for (auto it = var_type_names_.rbegin(); it != var_type_names_.rend(); ++it) {
        auto vit = it->find(name);
        if (vit != it->end()) return vit->second;
    }
    return "";
}

llvm::Value* CodeGenerator::lookup_var(const std::string& name, llvm::Type*& ty) {
    std::size_t idx = 0;
    return lookup_var_indexed(name, ty, idx);
}

llvm::Value* CodeGenerator::lookup_var_indexed(const std::string& name, llvm::Type*& ty, std::size_t& idx) {
    for (auto it = named_values.rbegin(); it != named_values.rend(); ++it) {
        auto vit = it->find(name);
        if (vit != it->end()) {
            idx = named_values.size() - 1 - std::distance(named_values.rbegin(), it);
            ty = named_types[idx][name];
            return vit->second;
        }
    }
    return nullptr;
}

bool CodeGenerator::is_ref_var(const std::string& name) {
    std::size_t idx = 0;
    llvm::Type* ty = nullptr;
    if (!lookup_var_indexed(name, ty, idx)) return false;
    return ref_scopes_[idx].count(name) > 0;
}

/**
 * Parses an Aevix type string (e.g., "int[5]") into base type and array size.
 * Accepts the primitive types plus every fixed-width type and registered
 * struct / enum names (enums are i32 values, so they parse like scalars).
 */
bool CodeGenerator::parse_type(const std::string& tn, std::string& base, int& arr_size) {
    base = tn;
    arr_size = -1;
    std::size_t br = tn.find('[');
    if (br != std::string::npos) {
        base = tn.substr(0, br);
        std::size_t close = tn.find(']', br);
        std::string sz = tn.substr(br + 1, close - br - 1);
        arr_size = sz.empty() ? 0 : std::stoi(sz);
    }
    if (is_numeric_type_name(base) || base == "bool" || base == "string") return true;
    if (struct_types.find(base) != struct_types.end()) return true;
    if (enum_names_.find(base) != enum_names_.end()) return true;
    return false;
}

llvm::Type* CodeGenerator::scalar_type_for(const std::string& base) {
    if (base == "int" || base == "i32") return builder->getInt32Ty();
    if (base == "i8" || base == "u8") return builder->getInt8Ty();
    if (base == "i16" || base == "u16") return builder->getInt16Ty();
    if (base == "i64" || base == "u64") return builder->getInt64Ty();
    if (base == "u32") return builder->getInt32Ty();
    if (base == "float" || base == "f64") return builder->getDoubleTy();
    if (base == "f32") return builder->getFloatTy();
    if (base == "bool") return builder->getInt1Ty();
    if (base == "string") return builder->getInt8Ty(); // legacy; string is normally a slice
    if (enum_names_.find(base) != enum_names_.end()) return builder->getInt32Ty();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Type-name classification (mirrors the sema rules in sema/sema.cpp)
// ---------------------------------------------------------------------------

// Base type of a (possibly array/slice) type string.
static std::string base_type_of(const std::string& tn) {
    std::size_t br = tn.find('[');
    return br == std::string::npos ? tn : tn.substr(0, br);
}

bool CodeGenerator::is_int_type_name(const std::string& tn) {
    const std::string b = base_type_of(tn);
    return b == "int" || b == "i8" || b == "i16" || b == "i32" || b == "i64"
        || b == "u8" || b == "u16" || b == "u32" || b == "u64";
}

bool CodeGenerator::is_float_type_name(const std::string& tn) {
    const std::string b = base_type_of(tn);
    return b == "float" || b == "f32" || b == "f64";
}

bool CodeGenerator::is_numeric_type_name(const std::string& tn) {
    return is_int_type_name(tn) || is_float_type_name(tn);
}

int CodeGenerator::type_width_bits(const std::string& tn) {
    const std::string b = base_type_of(tn);
    if (b == "i16" || b == "u16") return 16;
    if (b == "i32" || b == "u32" || b == "int") return 32;
    if (b == "i64" || b == "u64") return 64;
    if (b == "i8" || b == "u8") return 8;
    return -1;
}

int CodeGenerator::float_width_bits(const std::string& tn) {
    return base_type_of(tn) == "f32" ? 32 : 64; // float/f64
}

bool CodeGenerator::type_is_unsigned(const std::string& tn) {
    const std::string b = base_type_of(tn);
    return b == "u8" || b == "u16" || b == "u32" || b == "u64";
}

// Implicit coercions are widening-only: same type, int to a wider int, int to
// any float, or float to a wider float. Narrowing and every non-numeric
// cross-type assignment require an explicit to_* / sema check.
bool CodeGenerator::can_implicit_coerce(const std::string& src, const std::string& dest) {
    if (src == dest) return true;
    if (!is_numeric_type_name(src) || !is_numeric_type_name(dest)) return false;
    if (is_int_type_name(src) && is_int_type_name(dest))
        return type_width_bits(dest) > type_width_bits(src);
    if (is_int_type_name(src) && is_float_type_name(dest)) return true;
    if (is_float_type_name(src) && is_float_type_name(dest))
        return float_width_bits(dest) >= float_width_bits(src);
    return false; // float -> int is narrowing and requires an explicit to_*
}

llvm::Type* CodeGenerator::llvm_type_for(const std::string& tn) {
    return llvm_type_from_name(tn);
}

llvm::Type* CodeGenerator::signature_type_for(const std::string& tn, const std::string& ctx) {
    if (tn.empty()) return nullptr;
    std::string base;
    int arr_size = -1;
    if (!parse_type(tn, base, arr_size)) {
        error("Unknown type '" + tn + "' in " + ctx);
    }
    if (struct_types.find(base) != struct_types.end()) {
        if (arr_size > 0) return llvm::ArrayType::get(struct_types[base], arr_size);
        return struct_types[base];
    }
    if (base == "string") {
        // string = a slice of i8; string[N] = fixed arrays of those slices;
        // string[] = a slice whose elements are string slices (one layer of
        // indirection deeper than i8).
        if (arr_size > 0) return llvm::ArrayType::get(slice_type_for("string"), arr_size);
        if (arr_size == 0) return slice_type_for("string[]");
        return slice_type_for("string");
    }
    llvm::Type* b = scalar_type_for(base);
    if (arr_size >= 0) {
        if (arr_size == 0) {
            // Open array (int[]) in signature context: a slice { base*, i32 }
            return slice_type_for(base);
        }
        return llvm::ArrayType::get(b, arr_size);
    }
    return b;
}

bool CodeGenerator::is_struct(const std::string& tn) {
    return struct_types.find(tn) != struct_types.end();
}

llvm::StructType* CodeGenerator::struct_type_for(const std::string& tn) {
    auto it = struct_types.find(tn);
    if (it != struct_types.end()) return it->second;
    return nullptr;
}

int CodeGenerator::struct_field_index(const std::string& st, const std::string& field) {
    auto it = struct_field_indices.find(st);
    if (it == struct_field_indices.end()) return -1;
    auto fit = it->second.find(field);
    if (fit == it->second.end()) return -1;
    return fit->second;
}

llvm::Type* CodeGenerator::struct_field_type(const std::string& st, const std::string& field) {
    int idx = struct_field_index(st, field);
    if (idx < 0) return nullptr;
    return struct_type_for(st)->getElementType(idx);
}

void CodeGenerator::register_struct(const StructDecl& sd) {
    if (struct_types.find(sd.name) != struct_types.end()) return;
    struct_decls[sd.name] = &sd;
    std::vector<llvm::Type*> field_types;
    for (const auto& f : sd.fields) {
        std::string fb;
        int fsz = -1;
        parse_type(f.var_type, fb, fsz);
        field_types.push_back(llvm_type_from_name(f.var_type));
    }
    auto st = llvm::StructType::create(*context, field_types, sd.name);
    struct_types[sd.name] = st;
    std::map<std::string, int> idx_map;
    std::map<std::string, std::string> fname_map;
    for (std::size_t i = 0; i < sd.fields.size(); ++i) {
        idx_map[sd.fields[i].name] = (int)i;
        fname_map[sd.fields[i].name] = sd.fields[i].var_type;
    }
    struct_field_indices[sd.name] = idx_map;
    field_type_names_[sd.name] = fname_map;
}

void CodeGenerator::register_enum(const EnumDecl& ed) {
    if (enum_names_.find(ed.name) != enum_names_.end()) return;
    enum_names_.insert(ed.name);
    enum_variants_[ed.name] = ed.variants;
    for (std::size_t i = 0; i < ed.variants.size(); ++i) {
        enum_variant_indices_[ed.name + "." + ed.variants[i]] = (int)i;
    }
}

bool CodeGenerator::is_enum(const std::string& name) {
    return enum_names_.find(name) != enum_names_.end();
}

bool CodeGenerator::is_enum_variant(const std::string& en, const std::string& variant) {
    auto it = enum_variants_.find(en);
    if (it == enum_variants_.end()) return false;
    for (const auto& v : it->second) if (v == variant) return true;
    return false;
}

int CodeGenerator::enum_variant_index(const std::string& en, const std::string& variant) {
    auto it = enum_variant_indices_.find(en + "." + variant);
    return it == enum_variant_indices_.end() ? -1 : it->second;
}

bool CodeGenerator::is_enum_variant_expr(const std::shared_ptr<Expr>& e) {
    auto ma = std::dynamic_pointer_cast<MemberAccess>(e);
    if (!ma) return false;
    auto v = std::dynamic_pointer_cast<Variable>(ma->object);
    if (!v || !is_enum(v->name)) return false;
    return is_enum_variant(v->name, ma->member);
}

// Helper to convert an Aevix type string (scalar/array/struct) to an LLVM type.
llvm::Type* CodeGenerator::llvm_type_from_name(const std::string& tn) {
    std::string base;
    int arr_size = -1;
    parse_type(tn, base, arr_size);
    if (base == "string") {
        // string = a slice of i8; string[N] = fixed arrays of those slices;
        // string[] = a slice whose elements are string slices.
        if (arr_size > 0) return llvm::ArrayType::get(slice_type_for("string"), arr_size);
        if (arr_size == 0) return slice_type_for("string[]");
        return slice_type_for("string");
    }
    if (arr_size == 0) {
        // open array (int[]) maps to a slice struct { base*, i32 }
        return slice_type_for(base);
    }
    if (arr_size > 0) {
        llvm::Type* sc = scalar_type_for(base);
        if (sc) return llvm::ArrayType::get(sc, arr_size);
    }
    llvm::Type* sc = scalar_type_for(base);
    if (sc) return sc;
    auto sit = struct_types.find(base);
    if (sit != struct_types.end()) return sit->second;
    return llvm::Type::getVoidTy(*context);
}

llvm::Value* CodeGenerator::build_struct_literal(const StructLiteral& sl) {
    auto it = struct_types.find(sl.name);
    if (it == struct_types.end()) error("Unknown struct: " + sl.name);
    llvm::StructType* st = it->second;
    auto alloc = builder->CreateAlloca(st, nullptr, sl.name + ".lit");
    std::size_t n = st->getNumElements();
    if (sl.args.size() != n) error("Struct '" + sl.name + "' expects " + std::to_string(n) + " fields, got " + std::to_string(sl.args.size()));
    for (std::size_t i = 0; i < n; ++i) {
        llvm::Value* v = generate_expr(sl.args[i]);
        if (!v) return nullptr;
        llvm::Type* fty = st->getElementType(i);
        if (fty->isDoubleTy() && v->getType()->isIntegerTy(32)) {
            v = builder->CreateSIToFP(v, builder->getDoubleTy(), "cast");
        } else if (is_slice_ty(fty)) {
            v = coerce_to_slice(v, llvm::cast<llvm::StructType>(fty),
                "Type mismatch for field " + std::to_string(i) + " of struct '" + sl.name + "'");
        } else if (fty != v->getType()) {
            error("Type mismatch for field " + std::to_string(i) + " of struct '" + sl.name + "'");
        }
        llvm::Value* fptr = builder->CreateInBoundsGEP(st, alloc, {builder->getInt32(0), builder->getInt32((int)i)}, "f");
        builder->CreateStore(v, fptr);
    }
    return builder->CreateLoad(st, alloc, sl.name + ".val");
}

// Get pointer to a struct field (for read or write). Returns the field's type.
llvm::Value* CodeGenerator::gen_member_ptr(const MemberAccess& ma) {
    llvm::Type* fty = nullptr;
    return gen_member_ptr_inner(ma, fty);
}

llvm::Value* CodeGenerator::gen_member_ptr_inner(const MemberAccess& ma, llvm::Type*& field_ty) {
    llvm::Type* st_ty = nullptr;
    llvm::Value* base = nullptr;

    if (auto var = std::dynamic_pointer_cast<Variable>(ma.object)) {
        base = lookup_var(var->name, st_ty);
        if (!base) error("Variable not found: " + var->name);
        if (is_ref_var(var->name)) {
            base = builder->CreateLoad(builder->getPtrTy(), base, var->name + ".ref");
        }
        if (!st_ty->isStructTy()) error("'." + ma.member + "' on a non-struct value");
    } else if (auto inner = std::dynamic_pointer_cast<MemberAccess>(ma.object)) {
        llvm::Type* inner_ty = nullptr;
        base = gen_member_ptr_inner(*inner, inner_ty);
        st_ty = inner_ty;
        if (!st_ty->isStructTy()) error("'." + ma.member + "' on a non-struct value");
    } else if (auto ix = std::dynamic_pointer_cast<Index>(ma.object)) {
        llvm::Type* elem_ty = nullptr;
        base = gen_index_ptr(*ix, elem_ty);
        st_ty = elem_ty;
        if (!st_ty->isStructTy()) error("'." + ma.member + "' on a non-struct value");
    } else {
        error("Invalid member access target");
    }

    int idx = struct_field_index(st_ty->getStructName().str(), ma.member);
    if (idx < 0) error("Struct '" + st_ty->getStructName().str() + "' has no field '" + ma.member + "'");
    field_ty = llvm::cast<llvm::StructType>(st_ty)->getElementType(idx);
    return builder->CreateInBoundsGEP(st_ty, base, {builder->getInt32(0), builder->getInt32(idx)}, "field");
}

bool CodeGenerator::is_numeric(llvm::Type* ty) {
    if (ty->isIntegerTy() && !ty->isIntegerTy(1)) return true; // all int widths
    return ty->isFloatTy() || ty->isDoubleTy();
}

void CodeGenerator::promote_binop_operands(llvm::Value*& left, llvm::Value*& right) {
    if (left->getType()->isIntegerTy(8)) left = builder->CreateZExt(left, builder->getInt32Ty(), "char.up");
    if (right->getType()->isIntegerTy(8)) right = builder->CreateZExt(right, builder->getInt32Ty(), "char.up");
    if (left->getType()->isIntegerTy(32) && right->getType()->isDoubleTy())
        left = builder->CreateSIToFP(left, builder->getDoubleTy(), "tofloat");
    else if (right->getType()->isIntegerTy(32) && left->getType()->isDoubleTy())
        right = builder->CreateSIToFP(right, builder->getDoubleTy(), "tofloat");
}

void CodeGenerator::promote_binop_operands(llvm::Value*& left, llvm::Value*& right,
                                           const std::string& lname, const std::string& rname) {
    std::string target = common_numeric_name(lname, rname);
    if (target.empty()) {
        // Fallback: fall back to old LLVM-based promotion for values whose
        // type names are unknown (e.g. nested expressions, cached temporaries).
        if (left->getType()->isIntegerTy(8)) left = builder->CreateZExt(left, builder->getInt32Ty(), "char.up");
        if (right->getType()->isIntegerTy(8)) right = builder->CreateZExt(right, builder->getInt32Ty(), "char.up");
        // Mixed-signedness compare/arith with a bare int literal: the small
        // operand must be widened to i32 before the (signed) instruction.
        if (left->getType()->isIntegerTy(16) && right->getType()->isIntegerTy(32))
            left = type_is_unsigned(lname) ? builder->CreateZExt(left, builder->getInt32Ty(), "u16.up")
                                           : builder->CreateSExt(left, builder->getInt32Ty(), "i16.up");
        else if (right->getType()->isIntegerTy(16) && left->getType()->isIntegerTy(32))
            right = type_is_unsigned(rname) ? builder->CreateZExt(right, builder->getInt32Ty(), "u16.up")
                                            : builder->CreateSExt(right, builder->getInt32Ty(), "i16.up");
        if (left->getType()->isIntegerTy(32) && right->getType()->isDoubleTy())
            left = builder->CreateSIToFP(left, builder->getDoubleTy(), "tofloat");
        else if (right->getType()->isIntegerTy(32) && left->getType()->isDoubleTy())
            right = builder->CreateSIToFP(right, builder->getDoubleTy(), "tofloat");
        return;
    }
    left  = coerce_numeric(left,  lname, target);
    right = coerce_numeric(right, rname, target);
}

void CodeGenerator::require_numeric(llvm::Value* v, const std::string& ctx) {
    if (!is_numeric(v->getType())) {
        error("Operator " + ctx + " requires numeric operands");
    }
}

void CodeGenerator::require_bool(llvm::Value* v, const std::string& ctx) {
    require_bool_type(v->getType(), ctx);
}

void CodeGenerator::require_bool_type(llvm::Type* ty, const std::string& ctx) {
    if (!ty->isIntegerTy(1)) {
        error(ctx + " requires a bool condition");
    }
}

std::string CodeGenerator::llvm_type_name(llvm::Type* ty) {
    if (ty->isIntegerTy(8)) return "string char";
    if (ty->isIntegerTy(16)) return "i16";
    if (ty->isIntegerTy(32)) return "int";
    if (ty->isIntegerTy(64)) return "i64";
    if (ty->isFloatTy()) return "f32";
    if (ty->isDoubleTy()) return "float";
    if (ty->isIntegerTy(1)) return "bool";
    if (is_string_slice(ty)) return "string";
    if (ty->isPointerTy()) return "string";
    if (ty->isArrayTy()) return "array";
    if (ty->isStructTy()) return ty->getStructName().str();
    return "unknown";
}

// ============================================================================
// Enum values are represented as i32 indices, computed as the declaration
// order in the enum type.
// ============================================================================

// Canonical function type name: declared Aevix type strings are already
// canonical ("i64", "Color", "i64[]", ...), so this is the identity.
std::string CodeGenerator::fn_type_name(const std::string& tn) {
    return tn;
}

bool CodeGenerator::is_slice_dest(const std::string& tn) {
    if (tn.empty()) return false;
    if (tn.find("[]") != std::string::npos) return true;
    // A plain "string" destination is an i8 slice; a fixed "string[3]" is a
    // real array of strings and must NOT be treated as a slice destination.
    return tn == "string";
}

std::string CodeGenerator::lltype_to_name(llvm::Type* ty) {
    if (ty->isIntegerTy(1)) return "bool";
    if (ty->isIntegerTy(8)) return "i8";
    if (ty->isIntegerTy(16)) return "i16";
    if (ty->isIntegerTy(32)) return "int";
    if (ty->isIntegerTy(64)) return "i64";
    if (ty->isFloatTy()) return "f32";
    if (ty->isDoubleTy()) return "float";
    if (is_string_slice(ty)) return "string";
    if (is_slice_ty(ty)) {
        auto it = slice_bases.find(llvm::cast<llvm::StructType>(ty));
        if (it != slice_bases.end()) return it->second + "[]";
        return "unknown[]";
    }
    if (ty->isPointerTy()) return "string";
    if (ty->isArrayTy()) {
        auto at = llvm::cast<llvm::ArrayType>(ty);
        return lltype_to_name(at->getElementType()) + "[" +
               std::to_string(at->getNumElements()) + "]";
    }
    if (ty->isStructTy()) return ty->getStructName().str();
    return "unknown";
}

// Mirrors sema's common_numeric (sema/sema.cpp:69): the result type of an
// arithmetic op, or "" when the operand kinds clash (mixed signedness).
std::string CodeGenerator::common_numeric_name(const std::string& a, const std::string& b) {
    if (a == b) return a;
    if (is_float_type_name(a)) return is_float_type_name(b) ? (float_width_bits(a) >= float_width_bits(b) ? a : b) : a;
    if (is_float_type_name(b)) return b;
    if (is_int_type_name(a) && is_int_type_name(b)) {
        if (type_is_unsigned(a) != type_is_unsigned(b)) return ""; // mixed signedness
        return type_width_bits(a) >= type_width_bits(b) ? a : b;
    }
    return "";
}

// Element type name reached by indexing a value whose type name is `tn`.
// A bare "string" (a slice of i8) yields raw i8; an array/slice *of* strings
// yields "string" (the element is itself a slice).
static std::string index_elem_type_name(const std::string& tn) {
    if (tn.empty()) return "";
    bool has_bracket = tn.find('[') != std::string::npos;
    std::string el = base_type_of(tn);
    if (!has_bracket && el == "string") return "i8";
    return el;
}

std::string CodeGenerator::expr_type_name(const std::shared_ptr<Expr>& e) {
    if (!e) return "";
    if (auto n = std::dynamic_pointer_cast<Number>(e)) return "int";
    if (auto f = std::dynamic_pointer_cast<Float>(e)) return "float";
    if (auto b = std::dynamic_pointer_cast<Bool>(e)) return "bool";
    if (auto s = std::dynamic_pointer_cast<String>(e)) return "string";
    if (auto var = std::dynamic_pointer_cast<Variable>(e)) {
        std::string tn = lookup_var_type_name(var->name);
        if (!tn.empty()) return tn;
        if (is_open_array(var->name)) {
            auto oit = open_array_elem_type_names.find(var->name);
            if (oit != open_array_elem_type_names.end()) return base_type_of(oit->second) + "[]";
        }
        llvm::Type* ty = nullptr;
        if (lookup_var(var->name, ty)) return lltype_to_name(ty);
        return "";
    }
    if (auto al = std::dynamic_pointer_cast<ArrayLit>(e)) {
        if (al->elements.empty()) return "";
        std::string el = expr_type_name(al->elements[0]);
        if (el.empty()) return "";
        return el + "[" + std::to_string(al->elements.size()) + "]";
    }
    if (auto ix = std::dynamic_pointer_cast<Index>(e)) {
        return index_elem_type_name(expr_type_name(ix->object));
    }
    if (auto ma = std::dynamic_pointer_cast<MemberAccess>(e)) {
        if (is_enum_variant_expr(e)) {
            // Color.red: the object is the enum *name*, not an existing
            // variable, so report the enum type name directly.
            if (auto vobj = std::dynamic_pointer_cast<Variable>(ma->object))
                if (is_enum(vobj->name)) return vobj->name;
            return expr_type_name(ma->object);
        }
        std::string on = expr_type_name(ma->object);
        auto it = field_type_names_.find(base_type_of(on));
        if (it != field_type_names_.end()) {
            auto fit = it->second.find(ma->member);
            if (fit != it->second.end()) return fit->second;
        }
        return "";
    }
    if (auto sl = std::dynamic_pointer_cast<StructLiteral>(e)) return sl->name;
    if (auto n = std::dynamic_pointer_cast<New>(e)) return n->base + "[]";
    if (auto call = std::dynamic_pointer_cast<Call>(e)) {
        auto it = return_type_names_.find(call->callee);
        if (it != return_type_names_.end()) return it->second;
        return "";
    }
    if (auto neg = std::dynamic_pointer_cast<Neg>(e)) return expr_type_name(neg->value);
    if (auto add = std::dynamic_pointer_cast<Add>(e)) {
        std::string lt = expr_type_name(add->left), rt = expr_type_name(add->right);
        if (lt == "string" && rt == "string") return "string";
        return common_numeric_name(lt, rt);
    }
    if (auto sub = std::dynamic_pointer_cast<Sub>(e)) return common_numeric_name(expr_type_name(sub->left), expr_type_name(sub->right));
    if (auto mul = std::dynamic_pointer_cast<Mul>(e)) return common_numeric_name(expr_type_name(mul->left), expr_type_name(mul->right));
    if (auto div = std::dynamic_pointer_cast<Div>(e)) return common_numeric_name(expr_type_name(div->left), expr_type_name(div->right));
    if (std::dynamic_pointer_cast<CmpOp>(e)) return "bool";
    if (std::dynamic_pointer_cast<And>(e)) return "bool";
    if (std::dynamic_pointer_cast<Or>(e)) return "bool";
    if (std::dynamic_pointer_cast<Not>(e)) return "bool";
    return "";
}

// Emits a numeric conversion between two scalar type names (validated by sema).
llvm::Value* CodeGenerator::coerce_numeric(llvm::Value* v, const std::string& src, const std::string& dest) {
    if (src == dest || !v) return v;
    if (!is_numeric_type_name(src) || !is_numeric_type_name(dest)) {
        if (v->getType() != llvm_type_from_name(dest)) {
            error("Cannot convert " + src + " to " + dest);
        }
        return v;
    }
    bool src_int = is_int_type_name(src);
    bool dst_int = is_int_type_name(dest);
    bool src_float = is_float_type_name(src);
    bool dst_float = is_float_type_name(dest);

    if (src_int && dst_int) {
        int sw = type_width_bits(src), dw = type_width_bits(dest);
        if (dw > sw) {
            return type_is_unsigned(src) || type_is_unsigned(dest)
                ? builder->CreateZExt(v, llvm_type_from_name(dest), "zext")
                : builder->CreateSExt(v, llvm_type_from_name(dest), "sext");
        }
        if (dw < sw) return builder->CreateTrunc(v, llvm_type_from_name(dest), "trunc");
        return v; // same width (int vs i32/u32, i8 vs u8)
    }
    if (src_int && dst_float) {
        return type_is_unsigned(src)
            ? builder->CreateUIToFP(v, llvm_type_from_name(dest), "uitofp")
            : builder->CreateSIToFP(v, llvm_type_from_name(dest), "sitofp");
    }
    if (src_float && dst_int) {
        return type_is_unsigned(dest)
            ? builder->CreateFPToUI(v, llvm_type_from_name(dest), "fptoui")
            : builder->CreateFPToSI(v, llvm_type_from_name(dest), "fptosi");
    }
    if (src_float && dst_float) {
        int sw = float_width_bits(src), dw = float_width_bits(dest);
        if (dw > sw) return builder->CreateFPExt(v, llvm_type_from_name(dest), "fpext");
        if (dw < sw) return builder->CreateFPTrunc(v, llvm_type_from_name(dest), "fptrunc");
        return v;
    }
    error("Cannot convert " + src + " to " + dest);
}

// Builds a slice of `base` elements in the arena from an array literal,
// coercing each element to the base type. `base` is the *element* type name
// ("int", "i64", "Color", "string", ...).
llvm::Value* CodeGenerator::make_slice_from_lit(const ArrayLit& al, const std::string& base) {
    llvm::Type* elem = (base == "string")
        ? static_cast<llvm::Type*>(slice_type_for("string"))
        : scalar_type_for(base);
    if (!elem) error("Unknown element type in array literal: " + base);
    // A slice *of strings* is wrapped by string[]Slice, not the string slice.
    const std::string wrap = (base == "string") ? "string[]" : base;
    if (al.elements.empty()) {
        llvm::Value* nullp = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        return make_slice(nullp, wrap, elem, builder->getInt32(0));
    }
    uint64_t elem_bytes = module->getDataLayout().getTypeAllocSize(elem);
    llvm::Value* bytes64 = llvm::ConstantInt::get(builder->getInt64Ty(), elem_bytes * al.elements.size());
    llvm::Value* bytes32 = builder->CreateTrunc(bytes64, builder->getInt32Ty(), "bytes");
    llvm::Value* ptr = builder->CreateCall(alloc_func, {bytes32}, "lit.ptr");
    for (std::size_t i = 0; i < al.elements.size(); ++i) {
        const std::string en = expr_type_name(al.elements[i]);
        if (!en.empty() && en != base && !can_implicit_coerce(en, base))
            error("Array literal must contain a single type (got " + en + " and " + base + ")");
        llvm::Value* ev = coerce_expr_to(al.elements[i], base);
        if (!ev) return nullptr;
        llvm::Value* ep = builder->CreateInBoundsGEP(elem, ptr, {builder->getInt32((int)i)}, "lit.elem");
        builder->CreateStore(ev, ep);
    }
    return make_slice(ptr, wrap, elem, builder->getInt32((int)al.elements.size()));
}

// Coerces an expression to a named target type. Dest may be a slice ("int[]",
// "string"), a fixed array ("int[3]"), a scalar ("i64", "f32", "Color"), or
// an enum name. Array literals are rebuilt with per-element coercion and
// integer literals are materialized at the target width.
llvm::Value* CodeGenerator::coerce_expr_to(const std::shared_ptr<Expr>& e, const std::string& dest) {
    if (!e) return nullptr;
    std::string db = base_type_of(dest);
    int darr = -1;
    {
        std::string dbg;
        parse_type(dest, dbg, darr);
    }

    if (is_slice_dest(dest)) {
        if (auto al = std::dynamic_pointer_cast<ArrayLit>(e)) {
            // For "string[]" the element base is "string"; for "Color[]" it is
            // "Color". base_type_of already peeled the brackets.
            return make_slice_from_lit(*al, db == "string" ? "string" : db);
        }
        llvm::Value* v = generate_expr(e);
        if (!v) return nullptr;
        if (is_slice_ty(v->getType())) return v; // already a slice; sema checked it
        if (v->getType()->isArrayTy()) return copy_array_to_slice(v, slice_type_for(db == "string" ? "string[]" : db));
        return v;
    }

    if (darr > 0) {
        llvm::Type* elem_ty = nullptr;
        if (db == "string") {
            elem_ty = static_cast<llvm::Type*>(slice_type_for("string"));
        } else if (llvm::Type* st = scalar_type_for(db)) {
            elem_ty = st;
        } else {
            auto sit = struct_types.find(db);
            if (sit != struct_types.end()) elem_ty = sit->second;
        }
        if (!elem_ty) error("Unknown array type: " + dest);
        llvm::Type* arr_ty = llvm::ArrayType::get(elem_ty, darr);
        if (auto al = std::dynamic_pointer_cast<ArrayLit>(e)) {
            if (al->elements.size() != (std::size_t)darr) error("Array literal size mismatch");
            llvm::Value* tmp = builder->CreateAlloca(arr_ty, nullptr, "arr.coerce");
            for (int i = 0; i < darr; ++i) {
                const std::string en = expr_type_name(al->elements[i]);
                if (!en.empty() && en != db && !can_implicit_coerce(en, db))
                    error("Element type mismatch: cannot convert " + en + " to " + db
                          + " in array literal of " + dest);
                llvm::Value* ev = coerce_expr_to(al->elements[i], db);
                if (!ev) return nullptr;
                llvm::Value* ep = builder->CreateInBoundsGEP(arr_ty, tmp, {builder->getInt32(0), builder->getInt32(i)}, "arr.elem");
                builder->CreateStore(ev, ep);
            }
            return builder->CreateLoad(arr_ty, tmp, "arr.val");
        }
        llvm::Value* v = generate_expr(e);
        if (!v) return nullptr;
        if (v->getType() != arr_ty) error("Cannot assign " + lltype_to_name(v->getType()) + " to " + dest);
        return v;
    }

    // Scalar / struct / enum destination.
    const std::string src = expr_type_name(e);
    if (src == dest) return generate_expr(e);

    if (auto n = std::dynamic_pointer_cast<Number>(e)) {
        if (is_int_type_name(dest)) {
            return llvm::ConstantInt::get(llvm_type_from_name(dest),
                llvm::APInt(type_width_bits(dest), n->value));
        }
        if (is_float_type_name(dest)) {
            return llvm::ConstantFP::get(llvm_type_from_name(dest), (double)n->value);
        }
    }
    if (auto f = std::dynamic_pointer_cast<Float>(e)) {
        if (is_float_type_name(dest)) {
            return llvm::ConstantFP::get(llvm_type_from_name(dest), f->value);
        }
    }
    if (auto neg = std::dynamic_pointer_cast<Neg>(e)) {
        if (auto inner = std::dynamic_pointer_cast<Number>(neg->value)) {
            if (is_int_type_name(dest)) {
                llvm::APInt w(type_width_bits(dest), inner->value);
                w = -w;
                return llvm::ConstantInt::get(llvm_type_from_name(dest), w);
            }
        }
        if (auto finner = std::dynamic_pointer_cast<Float>(neg->value)) {
            if (is_float_type_name(dest)) {
                return llvm::ConstantFP::get(llvm_type_from_name(dest), -finner->value);
            }
        }
        llvm::Value* v = generate_expr(neg->value);
        if (!v) return nullptr;
        llvm::Value* negv = is_float_type_name(dest)
            ? builder->CreateFNeg(v, "negtmp")
            : builder->CreateNeg(v, "negtmp");
        return coerce_numeric(negv, src, dest);
    }

    llvm::Value* v = generate_expr(e);
    if (!v) return nullptr;
    std::string sname = src.empty() ? lltype_to_name(v->getType()) : src;
    if (sname == dest) return v;
    if (!can_implicit_coerce(sname, dest))
        error("Cannot convert " + sname + " to " + dest);
    return coerce_numeric(v, sname, dest);
}

// ============================================================================
// Array Handling
// ============================================================================

llvm::Value* CodeGenerator::gen_ref_arg_ptr(const std::shared_ptr<Expr>& e) {
    if (auto var = std::dynamic_pointer_cast<Variable>(e)) {
        llvm::Type* ty = nullptr;
        llvm::Value* alloc = lookup_var(var->name, ty);
        if (!alloc) error("Cannot resolve lvalue: " + var->name);
        if (is_ref_var(var->name))
            return builder->CreateLoad(builder->getPtrTy(), alloc, var->name + ".ref");
        return alloc;
    }
    if (auto ix = std::dynamic_pointer_cast<Index>(e)) {
        llvm::Type* elem_ty = nullptr;
        return gen_index_ptr(*ix, elem_ty);
    }
    if (auto ma = std::dynamic_pointer_cast<MemberAccess>(e)) {
        llvm::Type* fty = nullptr;
        return gen_member_ptr_inner(*ma, fty);
    }
    error("Cannot pass a temporary value to a ref parameter");
    return nullptr;
}

llvm::Type* CodeGenerator::element_type_of(const std::shared_ptr<Expr>& e) {
    if (auto n = std::dynamic_pointer_cast<Number>(e)) return builder->getInt32Ty();
    if (auto f = std::dynamic_pointer_cast<Float>(e)) return builder->getDoubleTy();
    if (auto b = std::dynamic_pointer_cast<Bool>(e)) return builder->getInt1Ty();
    if (auto s = std::dynamic_pointer_cast<String>(e)) return slice_type_for("string");
    if (auto al = std::dynamic_pointer_cast<ArrayLit>(e)) return build_array_type(*al);
    if (auto sl = std::dynamic_pointer_cast<StructLiteral>(e)) {
        auto it = struct_types.find(sl->name);
        if (it == struct_types.end()) error("Unknown struct: " + sl->name);
        return it->second;
    }
    if (auto ma = std::dynamic_pointer_cast<MemberAccess>(e)) {
        llvm::Type* fty = nullptr;
        gen_member_ptr_inner(*ma, fty);
        return fty;
    }
    if (auto var = std::dynamic_pointer_cast<Variable>(e)) {
        llvm::Type* t = nullptr;
        llvm::Value* alloc = lookup_var(var->name, t);
        if (!alloc) error("Variable not found: " + var->name);
        if (is_open_array(var->name)) return open_array_elem_type(var->name);
        if (!t->isArrayTy()) error("Variable '" + var->name + "' is not an array");
        return t;
    }
    error("Array elements must be literals of a single type");
}

std::string CodeGenerator::array_elem_type_name(const ArrayLit& al) {
    if (al.elements.empty()) error("Cannot infer the element type of an empty array ([])");
    std::string acc = expr_type_name(al.elements[0]);
    if (acc.empty()) error("Cannot infer the element type of an array literal");
    for (std::size_t i = 1; i < al.elements.size(); ++i) {
        std::string other = expr_type_name(al.elements[i]);
        if (other.empty()) error("Cannot infer the element type of an array literal");
        if (acc == other) continue;
        // Unify only same-family aliases ("int"/"i32", "float"/"f64"). A mixed
        // int+float literal is rejected: numeric literals never silently widen
        // inside an unchecked (inferred) array literal.
        bool same_family = is_float_type_name(acc) == is_float_type_name(other);
        bool same_shape = !is_float_type_name(acc)
            ? (type_width_bits(acc) == type_width_bits(other)
               && type_is_unsigned(acc) == type_is_unsigned(other))
            : (float_width_bits(acc) == float_width_bits(other));
        if (is_numeric_type_name(acc) && is_numeric_type_name(other)
            && same_family && same_shape) {
            acc = common_numeric_name(acc, other);
            if (acc.empty()) error("Array elements have incompatible numeric types");
            continue;
        }
        error("Array elements must share a single type (got " + acc + " and " + other + ")");
    }
    return acc;
}

llvm::Type* CodeGenerator::build_array_type(const ArrayLit& al) {
    return llvm::ArrayType::get(llvm_type_from_name(array_elem_type_name(al)), al.elements.size());
}

llvm::Constant* CodeGenerator::build_array_constant(const ArrayLit& al) {
    llvm::Type* arr_ty = build_array_type(al);
    std::vector<llvm::Constant*> cels;
    cels.reserve(al.elements.size());
    for (const auto& e : al.elements) {
        if (auto n = std::dynamic_pointer_cast<Number>(e)) cels.push_back(builder->getInt32(n->value));
        else if (auto f = std::dynamic_pointer_cast<Float>(e)) cels.push_back(llvm::ConstantFP::get(builder->getDoubleTy(), f->value));
        else if (auto b = std::dynamic_pointer_cast<Bool>(e)) cels.push_back(builder->getInt1(b->value ? 1 : 0));
        else if (auto s = std::dynamic_pointer_cast<String>(e)) {
            llvm::Constant* g = builder->CreateGlobalString(s->value, "str");
            cels.push_back(llvm::ConstantStruct::get(
                slice_type_for("string"),
                {g, builder->getInt32((int)s->value.size())}));
        }
        else if (auto inner = std::dynamic_pointer_cast<ArrayLit>(e)) cels.push_back(build_array_constant(*inner));
        else error("Array elements must be literals of a single type");
    }
    return llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(arr_ty), cels);
}

llvm::Value* CodeGenerator::gen_index_ptr(const Index& top, llvm::Type*& elem_ty) {
    std::vector<const Index*> chain;
    const Expr* cur = &top;
    while (auto ix = dynamic_cast<const Index*>(cur)) {
        chain.push_back(ix);
        cur = ix->object.get();
    }
    auto var = dynamic_cast<const Variable*>(cur);
    auto ma = dynamic_cast<const MemberAccess*>(cur);

    llvm::Type* arr_ty = nullptr;
    llvm::Value* ptr = nullptr;
    llvm::Type* cur_arr = nullptr;
    bool is_open = false;
    llvm::Value* open_len = nullptr;
    llvm::Type* open_elem = nullptr;

    if (var) {
        llvm::Value* alloc = lookup_var(var->name, arr_ty);
        if (!alloc) error("Variable not found: " + var->name);
        if (is_ref_var(var->name)) {
            // Ref/out storage holds the caller's array address: load it first.
            ptr = builder->CreateLoad(builder->getPtrTy(), alloc, var->name + ".ref");
            cur_arr = arr_ty;
            is_open = false;
        } else {
            ptr = alloc;
            cur_arr = arr_ty;
            is_open = is_open_array(var->name);
            if (is_open) {
                // The variable stores a slice { data*, len } — load the whole value.
                auto slice = builder->CreateLoad(arr_ty, alloc, var->name);
                ptr = builder->CreateExtractValue(slice, 0, var->name + ".ptr");
                open_len = builder->CreateExtractValue(slice, 1, var->name + ".len");
                open_elem = open_array_elem_type(var->name);
            } else {
                if (!arr_ty->isArrayTy()) error("Variable '" + var->name + "' is not an array");
            }
        }
    } else if (ma) {
        // Struct member access yielding a slice: b.items[0]
        llvm::Type* fty = nullptr;
        llvm::Value* fptr = gen_member_ptr_inner(*ma, fty);
        if (!is_slice_ty(fty)) error("Field '" + ma->member + "' is not an open array");
        auto slice = builder->CreateLoad(fty, fptr, ma->member + ".val");
        is_open = true;
        open_elem = slice_elem_type(llvm::cast<llvm::StructType>(fty));
        open_len = builder->CreateExtractValue(slice, 1, ma->member + ".len");
        ptr = builder->CreateExtractValue(slice, 0, ma->member + ".ptr");
    } else {
        error("Invalid array access target");
    }

    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        llvm::Value* idx = generate_expr((*it)->index);
        if (!idx) error("Invalid array index expression");
        if (!idx->getType()->isIntegerTy(32)) error("Array index must be an integer");
        int dim = is_open ? -1 : (int)cur_arr->getArrayNumElements();

        llvm::Value* neg = builder->CreateICmpSLT(idx, builder->getInt32(0));
        llvm::Value* huge;
        if (dim >= 0) {
            huge = builder->CreateICmpSGE(idx, builder->getInt32(dim));
        } else {
            huge = builder->CreateICmpSGE(idx, open_len);
        }
        llvm::Value* oob = builder->CreateOr(neg, huge, "oob");

        auto ok = llvm::BasicBlock::Create(*context, "idxok", builder->GetInsertBlock()->getParent());
        auto fail = llvm::BasicBlock::Create(*context, "idxfail", builder->GetInsertBlock()->getParent());
        builder->CreateCondBr(oob, fail, ok);
        builder->SetInsertPoint(fail);
        builder->CreateCall(oob_func, {idx, dim >= 0 ? builder->getInt32(dim) : open_len});
        builder->CreateUnreachable();

        builder->SetInsertPoint(ok);
        if (is_open) {
            ptr = builder->CreateInBoundsGEP(open_elem, ptr, {idx}, "idx");
        } else {
            ptr = builder->CreateInBoundsGEP(cur_arr, ptr, {builder->getInt32(0), idx}, "idx");
            cur_arr = cur_arr->getArrayElementType();
        }
    }
    elem_ty = is_open ? open_elem : cur_arr;
    return ptr;
}

// Allocates elem[sz] in the arena (zeroed) and returns a slice handle.
// Supports a constant size (new int[16]) or a runtime expression (new int[n]).
llvm::Value* CodeGenerator::gen_new(const New& n) {
    llvm::Type* elem;
    if (n.base == "string") {
        elem = slice_type_for("string");
    } else {
        elem = scalar_type_for(n.base);
        if (!elem) {
            auto it = struct_types.find(n.base);
            if (it == struct_types.end()) error("Unknown type in new: " + n.base);
            elem = it->second;
        }
    }

    llvm::Value* size = generate_expr(n.size);
    if (!size) error("new requires an explicit array size, e.g. new int[16]");
    if (size->getType()->isDoubleTy() || size->getType()->isIntegerTy(1)) {
        error("Array size for new must be an integer");
    }
    if (size->getType()->isIntegerTy(64)) {
        size = builder->CreateTrunc(size, builder->getInt32Ty(), "size32");
    }
    if (auto c = llvm::dyn_cast<llvm::ConstantInt>(size)) {
        if (c->getSExtValue() <= 0) error("new requires an explicit positive array size, e.g. new int[16]");
    }

    uint64_t elem_bytes = module->getDataLayout().getTypeAllocSize(elem);
    llvm::Value* sz64 = builder->CreateSExt(size, builder->getInt64Ty(), "sz64");
    llvm::Value* bytes64 = builder->CreateMul(sz64, builder->getInt64(elem_bytes), "bytes");
    llvm::Value* bytes32 = builder->CreateTrunc(bytes64, builder->getInt32Ty(), "bytes32");
    llvm::Value* ptr = builder->CreateCall(alloc_func, {bytes32}, "new.ptr");

    llvm::Function* memset_fn = llvm::Intrinsic::getOrInsertDeclaration(
        module.get(), llvm::Intrinsic::memset, llvm::Type::getVoidTy(*context),
        {llvm::PointerType::getUnqual(*context), builder->getInt8Ty(),
         builder->getInt64Ty(), builder->getInt1Ty()});
    builder->CreateCall(memset_fn, {ptr, builder->getInt8(0), bytes64, builder->getInt1(false)});

    return make_slice(ptr, n.base == "string" ? "string[]" : n.base, elem, size);
}

// Copies a stack array value into fresh arena memory, returning a slice.
// Used when an int[] function returns a fixed (stack) array literal/value.
llvm::Value* CodeGenerator::copy_array_to_slice(llvm::Value* arr_val, llvm::StructType* slice_ty) {
    auto at = llvm::cast<llvm::ArrayType>(arr_val->getType());
    llvm::Type* elem = at->getElementType();
    uint64_t elem_bytes = module->getDataLayout().getTypeAllocSize(elem);
    llvm::Value* bytes64 = llvm::ConstantInt::get(builder->getInt64Ty(), elem_bytes * at->getNumElements());
    llvm::Value* bytes32 = builder->CreateTrunc(bytes64, builder->getInt32Ty(), "bytes");
    llvm::Value* arena_ptr = builder->CreateCall(alloc_func, {bytes32}, "ret.ptr");
    for (uint64_t i = 0; i < at->getNumElements(); ++i) {
        llvm::Value* ep = builder->CreateInBoundsGEP(elem, arena_ptr, {builder->getInt32((uint32_t)i)}, "ret.elem");
        llvm::Value* ev = builder->CreateExtractValue(arr_val, i, "elem");
        builder->CreateStore(ev, ep);
    }
    return make_slice(arena_ptr, slice_bases[slice_ty], elem, builder->getInt32((int)at->getNumElements()));
}

// Brings a value into a slice form for a slice-typed destination (field, var,
// argument). Slices pass through unchanged; stack fixed arrays are copied into
// the arena so the slice owns its data instead of pointing into the caller's
// stack frame. Anything else is a type error.
llvm::Value* CodeGenerator::coerce_to_slice(llvm::Value* val, llvm::StructType* slice_ty, const std::string& ctx) {
    if (val->getType() == slice_ty) return val;
    if (val->getType()->isArrayTy()) {
        auto at = llvm::cast<llvm::ArrayType>(val->getType());
        if (at->getElementType() == slice_elem_type(slice_ty)) {
            return copy_array_to_slice(val, slice_ty);
        }
    }
    error(ctx);
    return nullptr;
}

// Emits a runtime loop that prints an open array as [a, b, c].
void CodeGenerator::emit_slice_print(llvm::Value* slice) {
    emit_slice_print(slice, true);
}

void CodeGenerator::emit_slice_print(llvm::Value* slice, bool trailing_newline) {
    llvm::Type* elem = slice_elem_type(llvm::cast<llvm::StructType>(slice->getType()));
    if (elem->isStructTy() && !is_string_slice(elem)) error("Cannot print an open array of structs yet");

    auto ptr_val = builder->CreateExtractValue(slice, 0, "sp.ptr");
    auto len_val = builder->CreateExtractValue(slice, 1, "sp.len");

    llvm::FunctionType* printf_ft = llvm::FunctionType::get(builder->getInt32Ty(), llvm::PointerType::getUnqual(*context), true);
    auto printf_func = module->getOrInsertFunction("printf", printf_ft);
    auto open_br = builder->CreateGlobalString("[", "open_br");
    builder->CreateCall(printf_func, {open_br});

    llvm::Function* func = builder->GetInsertBlock()->getParent();
    auto head = llvm::BasicBlock::Create(*context, "sp.head", func);
    auto body = llvm::BasicBlock::Create(*context, "sp.body", func);
    auto merge = llvm::BasicBlock::Create(*context, "sp.end");

    auto idx = builder->CreateAlloca(builder->getInt32Ty(), nullptr, "sp.i");
    builder->CreateStore(builder->getInt32(0), idx);
    builder->CreateBr(head);
    builder->SetInsertPoint(head);
    auto icur = builder->CreateLoad(builder->getInt32Ty(), idx, "sp.i.cur");
    auto cmp = builder->CreateICmpSLT(icur, len_val, "sp.cmp");
    builder->CreateCondBr(cmp, body, merge);

    builder->SetInsertPoint(body);
    auto isfirst = builder->CreateICmpEQ(icur, builder->getInt32(0), "sp.first");
    auto comma_block = llvm::BasicBlock::Create(*context, "sp.comma", func);
    auto nocomma_block = llvm::BasicBlock::Create(*context, "sp.nocomma", func);
    auto cont_block = llvm::BasicBlock::Create(*context, "sp.cont", func);
    builder->CreateCondBr(isfirst, nocomma_block, comma_block);
    builder->SetInsertPoint(comma_block);
    auto comma = builder->CreateGlobalString(", ", "comma");
    builder->CreateCall(printf_func, {comma});
    builder->CreateBr(cont_block);
    builder->SetInsertPoint(nocomma_block);
    builder->CreateBr(cont_block);
    builder->SetInsertPoint(cont_block);

    auto ep = builder->CreateInBoundsGEP(elem, ptr_val, {icur}, "sp.elem");
    auto ev = builder->CreateLoad(elem, ep, "sp.val");
    if (is_string_slice(elem)) {
        auto quote = builder->CreateGlobalString("\"", "sq");
        builder->CreateCall(printf_func, {quote});
        emit_print_scalar(ev, "string");
        builder->CreateCall(printf_func, {quote});
    }
    else if (elem->isStructTy()) {
        emit_print_struct_inline(ev, lltype_to_name(elem));
    }
    else {
        emit_print_scalar(ev, lltype_to_name(elem));
    }

    auto inext = builder->CreateAdd(icur, builder->getInt32(1), "sp.i.next");
    builder->CreateStore(inext, idx);
    builder->CreateBr(head);

    func->insert(func->end(), merge);
    builder->SetInsertPoint(merge);
    auto close_br = builder->CreateGlobalString(trailing_newline ? "]\n" : "]", "close_br");
    builder->CreateCall(printf_func, {close_br});
}

// ============================================================================
// Expressions: Literals, Arithmetic, Comparisons, Logic, Calls
// ============================================================================

llvm::Value* CodeGenerator::generate_expr(const std::shared_ptr<Expr>& expr) {
    if (!expr) return nullptr;
    if (expr->line > 0) {
        current_line = expr->line;
        current_col = expr->col;
    }

    if (auto num = std::dynamic_pointer_cast<Number>(expr)) return builder->getInt32(num->value);
    if (auto fl = std::dynamic_pointer_cast<Float>(expr)) return llvm::ConstantFP::get(builder->getDoubleTy(), fl->value);
    if (auto b = std::dynamic_pointer_cast<Bool>(expr)) return builder->getInt1(b->value ? 1 : 0);
    if (auto s = std::dynamic_pointer_cast<String>(expr)) {
        auto g = builder->CreateGlobalString(s->value, "str");
        return make_string_slice(g, builder->getInt32((int)s->value.size()));
    }
    if (auto var = std::dynamic_pointer_cast<Variable>(expr)) {
        llvm::Type* ty = nullptr;
        llvm::Value* alloc = lookup_var(var->name, ty);
        if (!alloc) error("Variable not found: " + var->name);
        llvm::Value* addr = alloc;
        if (is_ref_var(var->name)) {
            // Ref/out param storage holds the caller's address: load it first.
            addr = builder->CreateLoad(builder->getPtrTy(), alloc, var->name + ".ref");
        }
        return builder->CreateLoad(ty, addr, var->name);
    }
    if (auto al = std::dynamic_pointer_cast<ArrayLit>(expr)) {
        std::string en = array_elem_type_name(*al);
        llvm::Type* arr_ty = build_array_type(*al);
        llvm::Value* tmp = builder->CreateAlloca(arr_ty, nullptr, "arr.tmp");
        for (std::size_t i = 0; i < al->elements.size(); ++i) {
            llvm::Value* ev = coerce_expr_to(al->elements[i], en);
            if (!ev) return nullptr;
            llvm::Value* eptr = builder->CreateInBoundsGEP(arr_ty, tmp, {builder->getInt32(0), builder->getInt32((int)i)}, "arr.elem");
            builder->CreateStore(ev, eptr);
        }
        return builder->CreateLoad(arr_ty, tmp, "arr.val");
    }
    if (auto ix = std::dynamic_pointer_cast<Index>(expr)) {
        llvm::Type* elem_ty = nullptr;
        llvm::Value* ptr = gen_index_ptr(*ix, elem_ty);
        return builder->CreateLoad(elem_ty, ptr, "idxtmp");
    }
    if (auto ma = std::dynamic_pointer_cast<MemberAccess>(expr)) {
        // Color.red: an enum variant is a compile-time i32 index.
        if (is_enum_variant_expr(expr)) {
            auto vobj = std::dynamic_pointer_cast<Variable>(ma->object);
            int idx = enum_variant_index(vobj->name, ma->member);
            if (idx < 0) error("Unknown enum variant: " + vobj->name + "." + ma->member);
            return builder->getInt32(idx);
        }
        llvm::Type* fty = nullptr;
        llvm::Value* ptr = gen_member_ptr_inner(*ma, fty);
        return builder->CreateLoad(fty, ptr, "field");
    }
    if (auto sl = std::dynamic_pointer_cast<StructLiteral>(expr)) {
        return build_struct_literal(*sl);
    }
    if (auto n = std::dynamic_pointer_cast<New>(expr)) {
        return gen_new(*n);
    }
    if (auto call = std::dynamic_pointer_cast<Call>(expr)) {
        // Handle len() builtin
        if (call->callee == "len" && call->args.size() == 1) {
            auto av = generate_expr(call->args[0]);
            if (!av) return nullptr;
            if (av->getType()->isArrayTy()) {
                return builder->getInt32((int)llvm::cast<llvm::ArrayType>(av->getType())->getNumElements());
            }
            if (is_slice_ty(av->getType())) {
                return builder->CreateExtractValue(av, 1, "len");
            }
            error("len() requires an array variable");
        }

        // argc() -> int (arguments after the program name)
        if (call->callee == "argc" && call->args.empty()) {
            auto n = builder->CreateLoad(builder->getInt32Ty(), argc_global, "argc");
            return builder->CreateSub(n, builder->getInt32(1), "nargs");
        }

        // arg(i) -> string: i-th CLI argument (arena copy)
        if (call->callee == "arg" && call->args.size() == 1) {
            auto idx = generate_expr(call->args[0]);
            if (!idx) return nullptr;
            if (!idx->getType()->isIntegerTy(32)) error("arg() requires an integer index");
            auto n = builder->CreateLoad(builder->getInt32Ty(), argc_global, "argc");
            auto last = builder->CreateSub(n, builder->getInt32(1), "last");
            auto neg = builder->CreateICmpSLT(idx, builder->getInt32(0));
            auto too = builder->CreateICmpSGE(idx, last);
            auto oob = builder->CreateOr(neg, too, "arg.oob");
            auto fn = builder->GetInsertBlock()->getParent();
            auto bad = llvm::BasicBlock::Create(*context, "arg.bad", fn);
            auto ok = llvm::BasicBlock::Create(*context, "arg.ok", fn);
            auto cont = llvm::BasicBlock::Create(*context, "arg.cont", fn);
            builder->CreateCondBr(oob, bad, ok);
            builder->SetInsertPoint(bad);
            auto empt = make_string_slice(builder->CreateGlobalString("", "arg.empty"), builder->getInt32(0));
            builder->CreateBr(cont);
            builder->SetInsertPoint(ok);
            auto argv_ptr = builder->CreateLoad(llvm::PointerType::getUnqual(*context)->getPointerTo(), argv_global, "arg.argv");
            auto slot = builder->CreateInBoundsGEP(llvm::PointerType::getUnqual(*context), argv_ptr,
                builder->CreateAdd(idx, builder->getInt32(1), "argv.idx"), "arg.slot");
            auto cstr = builder->CreateLoad(llvm::PointerType::getUnqual(*context), slot, "arg.cstr");
            auto clen = builder->CreateCall(strlen_func, {cstr}, "arg.clen");
            auto b32 = builder->CreateTrunc(builder->CreateZExt(clen, builder->getInt64Ty()), builder->getInt32Ty(), "arg.b32");
            auto buf = builder->CreateCall(alloc_func, {b32}, "arg.buf");
            llvm::Function* memcpy_fn = llvm::Intrinsic::getOrInsertDeclaration(
                module.get(), llvm::Intrinsic::memcpy, llvm::Type::getVoidTy(*context),
                {llvm::PointerType::getUnqual(*context), llvm::PointerType::getUnqual(*context),
                 builder->getInt64Ty(), builder->getInt1Ty()});
            builder->CreateCall(memcpy_fn, {buf, cstr, builder->CreateZExt(clen, builder->getInt64Ty(), "arg.cpy"), builder->getInt1(false)});
            auto real = make_string_slice(buf, clen);
            builder->CreateBr(cont);
            builder->SetInsertPoint(cont);
            llvm::Value* merged = builder->CreatePHI(slice_type_for("string"), 2, "arg.sel");
            auto merphi = llvm::cast<llvm::PHINode>(merged);
            merphi->addIncoming(empt, bad);
            merphi->addIncoming(real, ok);
            return merged;
        }

        // exit(n): terminate the program with a status code.
        if (call->callee == "exit" && call->args.size() == 1) {
            auto v = generate_expr(call->args[0]);
            if (!v) return nullptr;
            if (!v->getType()->isIntegerTy(32)) error("exit() requires an integer code");
            builder->CreateCall(exit_func, {v});
            builder->CreateUnreachable();
            auto dead = llvm::BasicBlock::Create(*context, "exit.dead", builder->GetInsertBlock()->getParent());
            builder->SetInsertPoint(dead);
            return nullptr;
        }

        // read(fn) -> string: whole file as an arena-backed string.
        if (call->callee == "read" && call->args.size() == 1) {
            auto av = generate_expr(call->args[0]);
            if (!av) return nullptr;
            if (!is_string_slice(av->getType())) error("read() requires a string path");
            auto path = builder->CreateExtractValue(av, 0, "read.path");
            auto p_out = builder->CreateAlloca(llvm::PointerType::getUnqual(*context), nullptr, "read.p");
            auto l_out = builder->CreateAlloca(builder->getInt32Ty(), nullptr, "read.l");
            builder->CreateCall(read_file_func, {path, p_out, l_out});
            auto rp = builder->CreateLoad(llvm::PointerType::getUnqual(*context), p_out, "read.rp");
            auto rl = builder->CreateLoad(builder->getInt32Ty(), l_out, "read.rl");
            return make_string_slice(rp, rl);
        }

        // write(fn, s) -> bool: write a string to a file; true on success.
        if (call->callee == "write" && call->args.size() == 2) {
            auto pathv = generate_expr(call->args[0]);
            auto datav = generate_expr(call->args[1]);
            if (!pathv || !datav) return nullptr;
            if (!is_string_slice(pathv->getType())) error("write() requires a string path");
            if (!is_string_slice(datav->getType())) error("write() requires a string payload");
            auto ok = builder->CreateCall(write_file_func,
                {builder->CreateExtractValue(pathv, 0, "w.path"),
                 builder->CreateExtractValue(datav, 0, "w.data"),
                 builder->CreateExtractValue(datav, 1, "w.len")}, "w.ok");
            return builder->CreateICmpNE(ok, builder->getInt32(0), "w.bool");
        }

        // input() -> string: one line from stdin (no trailing newline); "" on EOF.
        if (call->callee == "input" && call->args.empty()) {
            auto p_out = builder->CreateAlloca(llvm::PointerType::getUnqual(*context), nullptr, "in.p");
            auto l_out = builder->CreateAlloca(builder->getInt32Ty(), nullptr, "in.l");
            builder->CreateCall(read_line_func, {p_out, l_out});
            auto rp = builder->CreateLoad(llvm::PointerType::getUnqual(*context), p_out, "in.rp");
            auto rl = builder->CreateLoad(builder->getInt32Ty(), l_out, "in.rl");
            return make_string_slice(rp, rl);
        }

        // to_int(s) -> int: leading decimal number of a string slice; numeric values
        // are coerced to i32.
        if (call->callee == "to_int" && call->args.size() == 1) {
            auto sv = generate_expr(call->args[0]);
            if (!sv) return nullptr;
            if (is_string_slice(sv->getType())) {
                return builder->CreateCall(to_int_func,
                    {builder->CreateExtractValue(sv, 0, "ti.ptr"),
                     builder->CreateExtractValue(sv, 1, "ti.len")}, "to.int");
            }
            return coerce_numeric(sv,
                expr_type_name(call->args[0]),
                "int");
        }

        // to_float(s) -> float: leading floating-point number of a string slice;
        // numeric values are coerced to f64.
        if (call->callee == "to_float" && call->args.size() == 1) {
            auto sv = generate_expr(call->args[0]);
            if (!sv) return nullptr;
            if (is_string_slice(sv->getType())) {
                return builder->CreateCall(to_float_func,
                    {builder->CreateExtractValue(sv, 0, "tf.ptr"),
                     builder->CreateExtractValue(sv, 1, "tf.len")}, "to.float");
            }
            return coerce_numeric(sv,
                expr_type_name(call->args[0]),
                "float");
        }

        // to_str(int|float|fixed-width) -> string: format a number, matching
        // print's format; i64/u64/f32/enum are handled.
        if (call->callee == "to_str" && call->args.size() == 1) {
            auto nv = generate_expr(call->args[0]);
            if (!nv) return nullptr;
            std::string sn = expr_type_name(call->args[0]);
            auto p_out = builder->CreateAlloca(llvm::PointerType::getUnqual(*context), nullptr, "ts.p");
            auto l_out = builder->CreateAlloca(builder->getInt32Ty(), nullptr, "ts.l");
            if (sn == "int" || sn == "i32") {
                builder->CreateCall(int_to_str_func, {nv, p_out, l_out});
            } else if (sn == "i8" || sn == "i16") {
                builder->CreateCall(int_to_str_func,
                    {builder->CreateSExt(nv, builder->getInt32Ty(), "conv"), p_out, l_out});
            } else if (sn == "u8" || sn == "u16" || sn == "u32") {
                builder->CreateCall(u64_to_str_func,
                    {builder->CreateZExt(nv, builder->getInt64Ty(), "conv"), p_out, l_out});
            } else if (sn == "i64") {
                builder->CreateCall(i64_to_str_func, {nv, p_out, l_out});
            } else if (sn == "u64") {
                builder->CreateCall(u64_to_str_func, {nv, p_out, l_out});
            } else if (sn == "float" || sn == "f64") {
                builder->CreateCall(double_to_str_func, {nv, p_out, l_out});
            } else if (sn == "f32") {
                builder->CreateCall(double_to_str_func,
                    {builder->CreateFPExt(nv, builder->getDoubleTy(), "conv"), p_out, l_out});
            } else {
                error("to_str() requires a numeric argument");
            }
            auto rp = builder->CreateLoad(llvm::PointerType::getUnqual(*context), p_out, "ts.rp");
            auto rl = builder->CreateLoad(builder->getInt32Ty(), l_out, "ts.rl");
            return make_string_slice(rp, rl);
        }

        // substr(s, start, count) -> string: clamped segment copied to the arena.
        if (call->callee == "substr" && call->args.size() == 3) {
            auto sv = generate_expr(call->args[0]);
            auto stv = generate_expr(call->args[1]);
            auto cnv = generate_expr(call->args[2]);
            if (!sv || !stv || !cnv) return nullptr;
            if (!is_string_slice(sv->getType())) error("substr() requires a string");
            if (!stv->getType()->isIntegerTy(32)) error("substr() requires integer start");
            if (!cnv->getType()->isIntegerTy(32)) error("substr() requires integer count");
            auto p_out = builder->CreateAlloca(llvm::PointerType::getUnqual(*context), nullptr, "sb.p");
            auto l_out = builder->CreateAlloca(builder->getInt32Ty(), nullptr, "sb.l");
            builder->CreateCall(substr_func,
                {builder->CreateExtractValue(sv, 0, "sb.ptr"),
                 builder->CreateExtractValue(sv, 1, "sb.len"), stv, cnv, p_out, l_out});
            auto rp = builder->CreateLoad(llvm::PointerType::getUnqual(*context), p_out, "sb.rp");
            auto rl = builder->CreateLoad(builder->getInt32Ty(), l_out, "sb.rl");
            return make_string_slice(rp, rl);
        }

        // split(s, sep) -> string[]: split s on every occurrence of sep
        // (empty parts preserved); an empty sep yields [s].
        if (call->callee == "split" && call->args.size() == 2) {
            auto sv = generate_expr(call->args[0]);
            auto spv = generate_expr(call->args[1]);
            if (!sv || !spv) return nullptr;
            if (!is_string_slice(sv->getType())) error("split() requires a string");
            if (!is_string_slice(spv->getType())) error("split() separator must be a string");
            auto p_out = builder->CreateAlloca(llvm::PointerType::getUnqual(*context), nullptr, "sp.p");
            auto l_out = builder->CreateAlloca(builder->getInt32Ty(), nullptr, "sp.l");
            builder->CreateCall(split_func,
                {builder->CreateExtractValue(sv, 0, "sp.ptr"),
                 builder->CreateExtractValue(sv, 1, "sp.len"),
                 builder->CreateExtractValue(spv, 0, "sp.sp"),
                 builder->CreateExtractValue(spv, 1, "sp.slen"),
                 p_out, l_out});
            auto rp = builder->CreateLoad(llvm::PointerType::getUnqual(*context), p_out, "sp.rp");
            auto rl = builder->CreateLoad(builder->getInt32Ty(), l_out, "sp.rl");
            return make_string_array_slice(rp, rl);
        }

        // min(a, b) / max(a, b): int or float comparison via select.
        if ((call->callee == "min" || call->callee == "max") && call->args.size() == 2) {
            bool want_min = call->callee == "min";
            auto a = generate_expr(call->args[0]);
            auto b = generate_expr(call->args[1]);
            if (!a || !b) return nullptr;
            require_numeric(a, call->callee);
            require_numeric(b, call->callee);
            std::string an = expr_type_name(call->args[0]);
            std::string bn = expr_type_name(call->args[1]);
            promote_binop_operands(a, b, an, bn);
            if (a->getType()->isDoubleTy() || a->getType()->isFloatTy()) {
                auto cmp = want_min
                    ? builder->CreateFCmpOLT(a, b, "min.cmp")
                    : builder->CreateFCmpOGT(a, b, "max.cmp");
                return builder->CreateSelect(cmp, a, b, "mm.sel");
            }
            bool is_unsigned = type_is_unsigned(common_numeric_name(an, bn));
            auto cmp = want_min
                ? (is_unsigned ? builder->CreateICmpULT(a, b, "min.cmp")
                               : builder->CreateICmpSLT(a, b, "min.cmp"))
                : (is_unsigned ? builder->CreateICmpUGT(a, b, "max.cmp")
                               : builder->CreateICmpSGT(a, b, "max.cmp"));
            return builder->CreateSelect(cmp, a, b, "mm.sel");
        }

        // abs(n): absolute value for int (select/negate) or float (fabs).
        // Unsigned operands are already non-negative, so abs is the identity.
        if (call->callee == "abs" && call->args.size() == 1) {
            auto av = generate_expr(call->args[0]);
            if (!av) return nullptr;
            std::string tname = expr_type_name(call->args[0]);
            if (is_numeric_type_name(tname) && type_is_unsigned(tname)) return av;
            llvm::Type* ty = av->getType();
            if (ty->isIntegerTy()) {
                auto zero = llvm::ConstantInt::get(ty, 0);
                auto is_neg = builder->CreateICmpSLT(av, zero, "abs.neg");
                auto neg = builder->CreateSub(zero, av, "abs.negval");
                return builder->CreateSelect(is_neg, neg, av, "abs.sel");
            }
            if (ty->isDoubleTy() || ty->isFloatTy()) {
                auto fabs = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::fabs, {ty});
                return builder->CreateCall(fabs, {av}, "abs.f");
            }
            error("abs() requires an int or float");
        }

        auto it = functions.find(call->callee);
        if (it == functions.end()) error("Unknown function: " + call->callee);
        llvm::Function* func = it->second;
        std::vector<llvm::Value*> args;
        auto callee_params_it = func_params_.find(call->callee);
        for (std::size_t i = 0; i < call->args.size(); ++i) {
            bool is_ref = false;
            std::string ptn;
            if (callee_params_it != func_params_.end() && i < callee_params_it->second.size()) {
                is_ref = callee_params_it->second[i].is_ref;
                ptn = callee_params_it->second[i].var_type;
            }

            if (is_ref) {
                auto addr = gen_ref_arg_ptr(call->args[i]);
                if (!addr) return nullptr;
                args.push_back(addr);
                continue;
            }

            llvm::Value* v = nullptr;
            if (i < func->arg_size() && !ptn.empty()) {
                // Reject clear scalar/enum mismatches up front so the reported
                // error names the offending argument (array/slice coercions are
                // left to coerce_expr_to, which handles fixed↔slice moves).
                const std::string an = expr_type_name(call->args[i]);
                if (!an.empty() && an != ptn && an.find('[') == std::string::npos
                    && !can_implicit_coerce(an, ptn)) {
                    llvm::Type* param_ty = func->getArg(i)->getType();
                    std::string got = llvm_type_name(llvm_type_from_name(an));
                    error("Argument " + std::to_string(i + 1) + " of '" + call->callee
                          + "' expects " + llvm_type_name(param_ty) + ", got " + got);
                }
                // Coerce the argument to the declared parameter type name:
                // literal integers materialise at the right width, array
                // literals become slices in the arena, numeric widening and
                // fixed↔slice conversions all route through coerce_expr_to.
                v = coerce_expr_to(call->args[i], ptn);
                if (!v) return nullptr;
                llvm::Type* param_ty = func->getArg(i)->getType();
                if (is_slice_ty(param_ty)) {
                    if (!is_slice_ty(v->getType())) {
                        error("Cannot pass " + lltype_to_name(v->getType()) + " to open array parameter of '" + call->callee + "'");
                    }
                } else if (v->getType() != param_ty) {
                    error("Argument " + std::to_string(i + 1) + " of '" + call->callee + "' expects " + llvm_type_name(param_ty) + ", got " + llvm_type_name(v->getType()));
                }
            } else {
                v = generate_expr(call->args[i]);
                if (!v) return nullptr;
            }
            args.push_back(v);
        }
        if (func->getReturnType()->isVoidTy()) {
            builder->CreateCall(func, args);
            return nullptr;
        }
        return builder->CreateCall(func, args, "calltmp");
    }
    if (auto add = std::dynamic_pointer_cast<Add>(expr)) {
        auto left = generate_expr(add->left);
        auto right = generate_expr(add->right);
        if (!left || !right) return nullptr;
        if (is_string_slice(left->getType()) || is_string_slice(right->getType())) {
            if (!is_string_slice(left->getType()) || !is_string_slice(right->getType())) {
                error("Operator + on a string requires both operands to be strings");
            }
            return gen_string_concat(left, right);
        }
        require_numeric(left, "+");
        require_numeric(right, "+");
        promote_binop_operands(left, right, expr_type_name(add->left), expr_type_name(add->right));
        if (left->getType()->isDoubleTy() || left->getType()->isFloatTy()) {
            return builder->CreateFAdd(left, right, "addtmp");
        }
        return builder->CreateAdd(left, right, "addtmp");
    }
    if (auto sub = std::dynamic_pointer_cast<Sub>(expr)) {
        auto left = generate_expr(sub->left);
        auto right = generate_expr(sub->right);
        if (!left || !right) return nullptr;
        require_numeric(left, "-");
        require_numeric(right, "-");
        promote_binop_operands(left, right, expr_type_name(sub->left), expr_type_name(sub->right));
        if (left->getType()->isDoubleTy() || left->getType()->isFloatTy()) {
            return builder->CreateFSub(left, right, "subtmp");
        }
        return builder->CreateSub(left, right, "subtmp");
    }
    if (auto mul = std::dynamic_pointer_cast<Mul>(expr)) {
        auto left = generate_expr(mul->left);
        auto right = generate_expr(mul->right);
        if (!left || !right) return nullptr;
        require_numeric(left, "*");
        require_numeric(right, "*");
        promote_binop_operands(left, right, expr_type_name(mul->left), expr_type_name(mul->right));
        if (left->getType()->isDoubleTy() || left->getType()->isFloatTy()) {
            return builder->CreateFMul(left, right, "multmp");
        }
        return builder->CreateMul(left, right, "multmp");
    }
    if (auto div = std::dynamic_pointer_cast<Div>(expr)) {
        auto left = generate_expr(div->left);
        auto right = generate_expr(div->right);
        if (!left || !right) return nullptr;
        require_numeric(left, "/");
        require_numeric(right, "/");
        std::string dn = common_numeric_name(expr_type_name(div->left), expr_type_name(div->right));
        promote_binop_operands(left, right, expr_type_name(div->left), expr_type_name(div->right));
        if (left->getType()->isDoubleTy() || left->getType()->isFloatTy()) {
            return builder->CreateFDiv(left, right, "divtmp");
        }
        if (type_is_unsigned(dn)) return builder->CreateUDiv(left, right, "divtmp");
        return builder->CreateSDiv(left, right, "divtmp");
    }
    if (auto neg = std::dynamic_pointer_cast<Neg>(expr)) {
        auto val = generate_expr(neg->value);
        if (!val) return nullptr;
        require_numeric(val, "unary -");
        if (val->getType()->isDoubleTy()) return builder->CreateFNeg(val, "negtmp");
        return builder->CreateNeg(val, "negtmp");
    }
    if (auto cmp = std::dynamic_pointer_cast<CmpOp>(expr)) {
        auto left = generate_expr(cmp->left);
        auto right = generate_expr(cmp->right);
        if (!left || !right) return nullptr;
        if (is_string_slice(left->getType()) || is_string_slice(right->getType())) {
            if (!is_string_slice(left->getType()) || !is_string_slice(right->getType())) {
                error("Comparison operands must be both strings");
            }
            if (cmp->op != "==" && cmp->op != "!=") error("Strings support only == and != comparison");
            auto lp = builder->CreateExtractValue(left, 0, "cmp.lp");
            auto llen = builder->CreateExtractValue(left, 1, "cmp.llen");
            auto rp = builder->CreateExtractValue(right, 0, "cmp.rp");
            auto rlen = builder->CreateExtractValue(right, 1, "cmp.rlen");
            auto eq = builder->CreateCall(str_eq_func, {lp, llen, rp, rlen}, "cmpeq");
            if (cmp->op == "==") return eq;
            return builder->CreateXor(eq, builder->getInt1(true), "cmpne");
        }
        bool both_numeric = is_numeric(left->getType()) && is_numeric(right->getType());
        bool both_bool = left->getType()->isIntegerTy(1) && right->getType()->isIntegerTy(1);
        if (!both_numeric && !both_bool) error("Comparison operands must be both numeric or both bool");
        std::string lname = expr_type_name(cmp->left);
        std::string rname = expr_type_name(cmp->right);
        promote_binop_operands(left, right, lname, rname);
        if (left->getType()->isDoubleTy() || left->getType()->isFloatTy()) {
            return generate_fcmp(cmp->op, left, right);
        }
        bool is_unsigned = both_numeric && type_is_unsigned(common_numeric_name(lname, rname));
        return generate_icmp(cmp->op, left, right, is_unsigned);
    }
    if (auto not_ = std::dynamic_pointer_cast<Not>(expr)) {
        auto val = generate_expr(not_->value);
        if (!val) return nullptr;
        require_bool(val, "!");
        return builder->CreateXor(val, builder->getInt1(true), "nottmp");
    }
    if (auto and_ = std::dynamic_pointer_cast<And>(expr)) return generate_logical_and(*and_);
    if (auto or_ = std::dynamic_pointer_cast<Or>(expr)) return generate_logical_or(*or_);

    return nullptr;
}

llvm::Value* CodeGenerator::generate_logical_and(const And& and_) {
    auto lhs = generate_expr(and_.left);
    if (!lhs) return nullptr;
    require_bool(lhs, "&&");

    auto current = builder->GetInsertBlock();
    llvm::Function* func = current->getParent();
    auto rhs_block = llvm::BasicBlock::Create(*context, "and.rhs", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "and.merge");

    builder->CreateCondBr(lhs, rhs_block, merge_block);
    builder->SetInsertPoint(rhs_block);
    auto rhs = generate_expr(and_.right);
    if (!rhs) return nullptr;
    require_bool(rhs, "&&");
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(merge_block);

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
    llvm::PHINode* phi = builder->CreatePHI(builder->getInt1Ty(), 2, "andtmp");
    phi->addIncoming(builder->getFalse(), current);
    phi->addIncoming(rhs, rhs_block);
    return phi;
}

llvm::Value* CodeGenerator::generate_logical_or(const Or& or_) {
    auto lhs = generate_expr(or_.left);
    if (!lhs) return nullptr;
    require_bool(lhs, "||");

    auto current = builder->GetInsertBlock();
    llvm::Function* func = current->getParent();
    auto rhs_block = llvm::BasicBlock::Create(*context, "or.rhs", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "or.merge");

    builder->CreateCondBr(lhs, merge_block, rhs_block);
    builder->SetInsertPoint(rhs_block);
    auto rhs = generate_expr(or_.right);
    if (!rhs) return nullptr;
    require_bool(rhs, "||");
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(merge_block);

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
    llvm::PHINode* phi = builder->CreatePHI(builder->getInt1Ty(), 2, "ortmp");
    phi->addIncoming(builder->getTrue(), current);
    phi->addIncoming(rhs, rhs_block);
    return phi;
}

llvm::Value* CodeGenerator::generate_icmp(const std::string& op, llvm::Value* left, llvm::Value* right) {
    return generate_icmp(op, left, right, false);
}

llvm::Value* CodeGenerator::generate_icmp(const std::string& op, llvm::Value* left, llvm::Value* right, bool is_unsigned) {
    if (op == "==") return builder->CreateICmpEQ(left, right, "cmptmp");
    if (op == "!=") return builder->CreateICmpNE(left, right, "cmptmp");
    if (is_unsigned) {
        if (op == "<")  return builder->CreateICmpULT(left, right, "cmptmp");
        if (op == ">")  return builder->CreateICmpUGT(left, right, "cmptmp");
        if (op == "<=") return builder->CreateICmpULE(left, right, "cmptmp");
        if (op == ">=") return builder->CreateICmpUGE(left, right, "cmptmp");
    } else {
        if (op == "<")  return builder->CreateICmpSLT(left, right, "cmptmp");
        if (op == ">")  return builder->CreateICmpSGT(left, right, "cmptmp");
        if (op == "<=") return builder->CreateICmpSLE(left, right, "cmptmp");
        if (op == ">=") return builder->CreateICmpSGE(left, right, "cmptmp");
    }
    error("Unknown icmp operator: " + op);
}

llvm::Value* CodeGenerator::generate_fcmp(const std::string& op, llvm::Value* left, llvm::Value* right) {
    if (op == "==") return builder->CreateFCmpOEQ(left, right, "cmptmp");
    if (op == "!=") return builder->CreateFCmpONE(left, right, "cmptmp");
    if (op == "<")  return builder->CreateFCmpOLT(left, right, "cmptmp");
    if (op == ">")  return builder->CreateFCmpOGT(left, right, "cmptmp");
    if (op == "<=") return builder->CreateFCmpOLE(left, right, "cmptmp");
    if (op == ">=") return builder->CreateFCmpOGE(left, right, "cmptmp");
    error("Unknown fcmp operator: " + op);
}

// ============================================================================
// Statements: Declarations, IO, Control Flow, Assignments
// ============================================================================

void CodeGenerator::generate_let(const Let& let) {
    if (!let.var_type.empty()) {
        std::string base;
        int arr_size = -1;
        if (!parse_type(let.var_type, base, arr_size)) error("Unknown type: " + let.var_type);

        if (arr_size == 0 || is_slice_dest(let.var_type)) {
            // Open array let: let arr: int[] = <array literal | slice value>
            llvm::StructType* st = llvm::cast<llvm::StructType>(llvm_type_from_name(let.var_type));
            llvm::Value* val = coerce_expr_to(let.value, let.var_type);
            if (!val) return;
            auto alloc = builder->CreateAlloca(st, nullptr, let.name);
            builder->CreateStore(val, alloc);
            named_values.back()[let.name] = alloc;
            named_types.back()[let.name] = st;
            var_type_names_.back()[let.name] = let.var_type;
            register_open_array(let.name, slice_elem_type(st));
            // A plain "string" stores i8 chars; a "string[]" slice stores strings.
            std::string sb_ = base == "string" ? "i8" : base;
            open_array_elem_type_names[let.name] = sb_;
            open_epochs[let.name] = epoch_depth;
            return;
        }

        // Fixed array or scalar: coerce the value to the declared type.
        llvm::Value* val = coerce_expr_to(let.value, let.var_type);
        if (!val) return;
        llvm::Type* declared = arr_size == -1
            ? llvm_type_from_name(base)
            : llvm_type_from_name(let.var_type);
        auto alloc = builder->CreateAlloca(declared, nullptr, let.name);
        builder->CreateStore(val, alloc);
        if (arr_size > 0) {
            // Declared fixed arrays are not open arrays.
            define_var(let.name, alloc, declared, let.var_type);
            return;
        }
        var_type_names_.back()[let.name] = base;
        define_var(let.name, alloc, declared, base);
        return;
    }

    // Inference: let x = <expr> at the expression's semantic type.
    llvm::Value* val = generate_expr(let.value);
    if (!val) return;
    llvm::Type* ty = val->getType();
    std::string tname = expr_type_name(let.value);
    if (tname.empty()) tname = lltype_to_name(ty);

    if (is_slice_ty(ty)) {
        // Inference: let a = <slice expr>  (new / int[] call) → open array var
        auto alloc = builder->CreateAlloca(ty, nullptr, let.name);
        builder->CreateStore(val, alloc);
        named_values.back()[let.name] = alloc;
        named_types.back()[let.name] = ty;
        var_type_names_.back()[let.name] = tname;
        llvm::StructType* st = llvm::cast<llvm::StructType>(ty);
        std::string sbase = slice_bases[st];
        std::string elem_name =
            sbase == "string" ? "i8" : (sbase == "string[]" ? "string" : sbase);
        open_array_elem_type_names[let.name] = elem_name;
        register_open_array(let.name, slice_elem_type(st));
        open_epochs[let.name] = epoch_depth;
        return;
    }

    auto alloc = builder->CreateAlloca(ty, nullptr, let.name);
    builder->CreateStore(val, alloc);
    define_var(let.name, alloc, ty, tname);
}

llvm::Value* CodeGenerator::apply_compound(llvm::Value* old, llvm::Value* rhs,
                                           const std::string& op, llvm::Type* target_ty,
                                           const std::string& target_name,
                                           const std::string& rhs_name,
                                           const std::string& ctx, bool is_unsigned) {
    if (!is_numeric(target_ty)) {
        error("Operator '" + op + "' requires a numeric target (" + ctx + ")");
    }
    std::string tn = target_name.empty() ? lltype_to_name(target_ty) : target_name;
    if (is_unsigned) {
        // Recover the unsigned spelling before coercing the RHS; the LLVM
        // type alone cannot tell signed from unsigned.
        if (target_ty->isIntegerTy(8)) tn = "u8";
        else if (target_ty->isIntegerTy(16)) tn = "u16";
        else if (target_ty->isIntegerTy(32)) tn = "u32";
        else if (target_ty->isIntegerTy(64)) tn = "u64";
    }
    std::string rn = rhs_name.empty() ? lltype_to_name(rhs->getType()) : rhs_name;
    if (rn != tn && !can_implicit_coerce(rn, tn))
        error("Cannot convert " + rn + " to " + tn + " in operator '" + op + "' (" + ctx + ")");
    rhs = coerce_numeric(rhs, rn, tn);
    bool is_fp = is_float_type_name(tn);
    if (op == "+=") {
        return is_fp ? builder->CreateFAdd(old, rhs, "cmp.add")
                     : builder->CreateAdd(old, rhs, "cmp.add");
    }
    if (op == "-=") {
        return is_fp ? builder->CreateFSub(old, rhs, "cmp.sub")
                     : builder->CreateSub(old, rhs, "cmp.sub");
    }
    if (op == "*=") {
        return is_fp ? builder->CreateFMul(old, rhs, "cmp.mul")
                     : builder->CreateMul(old, rhs, "cmp.mul");
    }
    if (op == "/=") {
        if (is_fp) return builder->CreateFDiv(old, rhs, "cmp.div");
        return is_unsigned ? builder->CreateUDiv(old, rhs, "cmp.div")
                           : builder->CreateSDiv(old, rhs, "cmp.div");
    }
    error("Unknown assignment operator '" + op + "'");
}

void CodeGenerator::generate_assign(const Assign& a) {
    if (auto ix = std::dynamic_pointer_cast<Index>(a.name)) {
        llvm::Type* elem_ty = nullptr;
        llvm::Value* ptr = gen_index_ptr(*ix, elem_ty);
        llvm::Value* val = nullptr;
        if (a.op == "=") {
            val = generate_expr(a.value);
            if (!val) return;
            if (elem_ty->isDoubleTy() && val->getType()->isIntegerTy(32)) {
                val = builder->CreateSIToFP(val, builder->getDoubleTy(), "cast");
            } else if (elem_ty->isArrayTy()) {
                error("Cannot assign an array to a single array element");
            } else if (elem_ty != val->getType()) {
                error("Type mismatch for indexed assignment");
            }
        } else {
            auto old = builder->CreateLoad(elem_ty, ptr, "cmp.old");
            auto rhs = generate_expr(a.value);
            if (!rhs) return;
            std::string elem_name = index_elem_type_name(expr_type_name(ix->object));
            bool ui = is_numeric_type_name(elem_name) && type_is_unsigned(elem_name);
            val = apply_compound(old, rhs, a.op, elem_ty, elem_name,
                                 expr_type_name(a.value), "indexed assignment", ui);
        }
        builder->CreateStore(val, ptr);
        return;
    }

    if (auto ma = std::dynamic_pointer_cast<MemberAccess>(a.name)) {
        llvm::Type* fty = nullptr;
        llvm::Value* ptr = gen_member_ptr_inner(*ma, fty);
        llvm::Value* val = nullptr;
        if (a.op == "=") {
            val = generate_expr(a.value);
            if (!val) return;
            if (fty->isDoubleTy() && val->getType()->isIntegerTy(32)) {
                val = builder->CreateSIToFP(val, builder->getDoubleTy(), "cast");
            } else if (is_slice_ty(fty)) {
                val = coerce_to_slice(val, llvm::cast<llvm::StructType>(fty),
                    "Type mismatch for field assignment");
            } else if (fty != val->getType()) {
                error("Type mismatch for field assignment");
            }
        } else {
            auto old = builder->CreateLoad(fty, ptr, "cmp.old");
            auto rhs = generate_expr(a.value);
            if (!rhs) return;
            std::string fname;
            auto sit = field_type_names_.find(base_type_of(expr_type_name(ma->object)));
            if (sit != field_type_names_.end()) {
                auto fit = sit->second.find(ma->member);
                if (fit != sit->second.end()) fname = fit->second;
            }
            bool ui = is_numeric_type_name(fname) && type_is_unsigned(fname);
            val = apply_compound(old, rhs, a.op, fty, fname,
                                 expr_type_name(a.value), "field assignment", ui);
        }
        builder->CreateStore(val, ptr);
        return;
    }

    auto var = std::dynamic_pointer_cast<Variable>(a.name);
    if (!var) error("Invalid assignment target");

    llvm::Type* ty = nullptr;
    llvm::Value* alloc = lookup_var(var->name, ty);
    if (!alloc) error("Cannot assign to unknown variable: " + var->name);

    if (is_ref_var(var->name)) {
            // Write through a ref parameter: load the caller's address
            // and target that storage directly instead of the local slot.
            alloc = builder->CreateLoad(builder->getPtrTy(), alloc, var->name + ".ref");
        }

    if (a.op != "=") {
        if (is_slice_ty(ty)) {
            error("Operator '" + a.op + "' cannot be used on a slice (open array)");
        }
        auto old = builder->CreateLoad(ty, alloc, "cmp.old");
        auto rhs = generate_expr(a.value);
        if (!rhs) return;
        std::string vname = lookup_var_type_name(var->name);
        bool ui = type_is_unsigned(vname);
        llvm::Value* val = apply_compound(old, rhs, a.op, ty, vname,
                                          expr_type_name(a.value),
                                          "assignment to '" + var->name + "'", ui);
        builder->CreateStore(val, alloc);
        return;
    }

    llvm::Value* val = generate_expr(a.value);
    if (!val) return;

    llvm::Type* val_ty = val->getType();
    if (is_slice_ty(ty)) {
        // Assigning a slice: the target must live at or below the current epoch
        // depth, otherwise the arena rollback would leave it dangling.
        if (open_epochs[var->name] < epoch_depth) {
            error("Cannot move an arena slice out of its epoch: assignment to '" + var->name + "'");
        }
        if (val_ty != ty) {
            if (val_ty->isArrayTy()) {
                val = coerce_to_slice(val, llvm::cast<llvm::StructType>(ty),
                    "Type mismatch for '" + var->name + "': cannot assign " + llvm_type_name(val_ty) + " to " + llvm_type_name(ty));
            } else {
                error("Type mismatch for '" + var->name + "': cannot assign " + llvm_type_name(val_ty) + " to " + llvm_type_name(ty));
            }
        }
        builder->CreateStore(val, alloc);
        return;
    }
    if (ty->isDoubleTy() && val_ty->isIntegerTy(32)) {
        val = builder->CreateSIToFP(val, builder->getDoubleTy(), "cast");
    } else if (ty != val_ty) {
        error("Type mismatch for '" + var->name + "': cannot assign " + llvm_type_name(val_ty) + " to " + llvm_type_name(ty));
    }

    builder->CreateStore(val, alloc);
}

void CodeGenerator::generate_hot(const Hot& hot) {
    for (const auto& stmt : hot.body) generate_stmt(stmt);
}

void CodeGenerator::generate_epoch(const Epoch& ep) {
    auto save = builder->CreateCall(epoch_begin_func, {}, "epoch.save");
    epoch_depth++;
    push_scope();
    for (const auto& stmt : ep.body) generate_stmt(stmt);
    if (!block_has_terminator(builder->GetInsertBlock())) {
        builder->CreateCall(epoch_end_func, {save});
    }
    pop_scope();
    epoch_depth--;
}

void CodeGenerator::generate_print(const Print& print) {
    llvm::FunctionType* printf_ft = llvm::FunctionType::get(builder->getInt32Ty(), llvm::PointerType::getUnqual(*context), true);
    auto printf_func = module->getOrInsertFunction("printf", printf_ft);
    size_t n = print.args.size();
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) {
            auto sp = builder->CreateGlobalString(" ", "print.space");
            builder->CreateCall(printf_func, {sp});
        }
        emit_print_value(print.args[i]);
    }
    auto nl = builder->CreateGlobalString("\n", "print.nl");
    builder->CreateCall(printf_func, {nl});
}

void CodeGenerator::emit_print_scalar(llvm::Value* val, const std::string& aevix_name) {
    llvm::FunctionType* printf_ft = llvm::FunctionType::get(builder->getInt32Ty(), llvm::PointerType::getUnqual(*context), true);
    auto printf_func = module->getOrInsertFunction("printf", printf_ft);
    std::string base = base_type_of(aevix_name);

    // Enum: print the variant name selected by the i32 index.
    if (is_enum(base)) {
        if (!val->getType()->isIntegerTy(32)) error("Internal: enum value is not i32");
        auto it = enum_variants_.find(base);
        if (it == enum_variants_.end()) error("Internal: unknown enum " + base);
        llvm::Function* fn = builder->GetInsertBlock()->getParent();
        auto merge = llvm::BasicBlock::Create(*context, "enum.print.merge", fn);
        auto def = llvm::BasicBlock::Create(*context, "enum.print.def", fn);
        auto switch_inst = builder->CreateSwitch(val, def, (unsigned)it->second.size());
        for (std::size_t i = 0; i < it->second.size(); ++i) {
            auto cblk = llvm::BasicBlock::Create(*context, "enum.print.case", fn);
            switch_inst->addCase(builder->getInt32((int)i), cblk);
            builder->SetInsertPoint(cblk);
            auto nm = builder->CreateGlobalString(it->second[i], "enum.name");
            builder->CreateCall(printf_func, {nm});
            builder->CreateBr(merge);
        }
        builder->SetInsertPoint(def);
        auto fmt_num = builder->CreateGlobalString("%d", "fmt");
        builder->CreateCall(printf_func, {fmt_num, val});
        builder->CreateBr(merge);
        builder->SetInsertPoint(merge);
        return;
    }

    if (base == "i8" || base == "u8") {
        auto fmt = builder->CreateGlobalString("%c", "format");
        builder->CreateCall(printf_func, {fmt, builder->CreateZExt(val, builder->getInt32Ty())});
        return;
    }
    if (base == "int" || base == "i32" || base == "i16" || base == "u16") {
        llvm::Value* arg = val;
        if (!val->getType()->isIntegerTy(32)) {
            arg = base.at(0) == 'u'
                ? builder->CreateZExt(val, builder->getInt32Ty(), "uarg")
                : builder->CreateSExt(val, builder->getInt32Ty(), "promote");
        }
        auto fmt = builder->CreateGlobalString("%d", "format");
        builder->CreateCall(printf_func, {fmt, arg});
        return;
    }
    if (base == "u32") {
        auto arg = builder->CreateZExt(val, builder->getInt64Ty(), "u32arg");
        auto fmt = builder->CreateGlobalString("%llu", "format");
        builder->CreateCall(printf_func, {fmt, arg});
        return;
    }
    if (base == "i64") {
        auto fmt = builder->CreateGlobalString("%lld", "format");
        builder->CreateCall(printf_func, {fmt, val});
        return;
    }
    if (base == "u64") {
        auto fmt = builder->CreateGlobalString("%llu", "format");
        builder->CreateCall(printf_func, {fmt, val});
        return;
    }
    if (base == "float" || base == "f64") {
        if (!val->getType()->isDoubleTy()) error("Internal: float value is not f64");
        auto fmt = builder->CreateGlobalString("%f", "format");
        builder->CreateCall(printf_func, {fmt, val});
        return;
    }
    if (base == "f32") {
        auto arg = builder->CreateFPExt(val, builder->getDoubleTy(), "f32arg");
        auto fmt = builder->CreateGlobalString("%f", "format");
        builder->CreateCall(printf_func, {fmt, arg});
        return;
    }
    if (base == "bool") {
        auto tr = builder->CreateGlobalString("true", "bool.true");
        auto fl = builder->CreateGlobalString("false", "bool.false");
        auto fmt = builder->CreateGlobalString("%s", "format");
        builder->CreateCall(printf_func, {fmt, builder->CreateSelect(val, tr, fl)});
        return;
    }
    if (base == "string") {
        auto sp = builder->CreateExtractValue(val, 0, "sp.ptr");
        auto slen = builder->CreateExtractValue(val, 1, "sp.len");
        auto fmt = builder->CreateGlobalString("%.*s", "format");
        builder->CreateCall(printf_func, {fmt, slen, sp});
        return;
    }
    if (val->getType()->isIntegerTy(32)) {
        // Fallback: unknown i32 values print as signed int (e.g. u32 in an
        // array whose element type could not be resolved to its Aevix name).
        auto fmt = builder->CreateGlobalString("%d", "format");
        builder->CreateCall(printf_func, {fmt, val});
        return;
    }
    error("Cannot print a value of type " + aevix_name);
}

void CodeGenerator::emit_print_value(const std::shared_ptr<Expr>& value) {
    auto val = generate_expr(value);
    if (!val) return;

    llvm::Type* ty = val->getType();
    bool is_structured = ty->isArrayTy() || is_slice_ty(ty) || ty->isStructTy();
    if (!is_structured) {
        std::string aname = expr_type_name(value);
        if (aname.empty()) aname = lltype_to_name(ty);
        emit_print_scalar(val, aname);
        return;
    }

    llvm::FunctionType* printf_ft = llvm::FunctionType::get(builder->getInt32Ty(), llvm::PointerType::getUnqual(*context), true);
    auto printf_func = module->getOrInsertFunction("printf", printf_ft);

    if (val->getType()->isArrayTy()) {
        auto at = llvm::cast<llvm::ArrayType>(val->getType());
        int n = (int)at->getNumElements();
        llvm::Type* elem = at->getElementType();
        auto tmp = builder->CreateAlloca(at, nullptr, "print.arr");
        builder->CreateStore(val, tmp);
        auto open_br = builder->CreateGlobalString("[", "open_br");
        builder->CreateCall(printf_func, {open_br});
        for (int i = 0; i < n; ++i) {
            auto eptr = builder->CreateInBoundsGEP(at, tmp, {builder->getInt32(0), builder->getInt32(i)}, "print.elem");
            auto ev = builder->CreateLoad(elem, eptr, "print.val");
            if (i > 0) {
                auto comma = builder->CreateGlobalString(", ", "comma");
                builder->CreateCall(printf_func, {comma});
            }
            llvm::Value* fmt = nullptr;
            llvm::Value* a = ev;
            if (elem->isIntegerTy(32)) fmt = builder->CreateGlobalString("%d", "fmt");
            else if (elem->isDoubleTy()) fmt = builder->CreateGlobalString("%f", "fmt");
            else if (elem->isIntegerTy(1)) {
                auto tr = builder->CreateGlobalString("true", "bool.true");
                auto fl = builder->CreateGlobalString("false", "bool.false");
                fmt = builder->CreateGlobalString("%s", "fmt");
                a = builder->CreateSelect(ev, tr, fl);
            }
            else if (is_string_slice(elem)) {
                auto quote = builder->CreateGlobalString("\"", "sq");
                builder->CreateCall(printf_func, {quote});
                auto fp = builder->CreateExtractValue(ev, 0, "pa.ptr");
                auto fl = builder->CreateExtractValue(ev, 1, "pa.len");
                auto sf = builder->CreateGlobalString("%.*s", "sfmt");
                builder->CreateCall(printf_func, {sf, fl, fp});
                builder->CreateCall(printf_func, {quote});
                continue;
            }
            else if (elem->isStructTy()) {
                emit_print_struct_inline(ev, /*type_name=*/lltype_to_name(elem));
                continue;
            }
            emit_print_scalar(ev, lltype_to_name(elem));
        }
        auto close_br = builder->CreateGlobalString("]", "close_br");
        builder->CreateCall(printf_func, {close_br});
        return;
    }
    else if (is_slice_ty(val->getType())) {
        if (is_string_slice(val->getType())) {
            auto sp = builder->CreateExtractValue(val, 0, "sp.ptr");
            auto slen = builder->CreateExtractValue(val, 1, "sp.len");
            auto fmt = builder->CreateGlobalString("%.*s", "format");
            builder->CreateCall(printf_func, {fmt, slen, sp});
            return;
        }
        emit_slice_print(val, false);
        return;
    }
    else if (val->getType()->isStructTy()) {
        emit_print_struct_inline(val, lltype_to_name(val->getType()));
        return;
    }
    error("Cannot print a value of type " + llvm_type_name(val->getType()));
}

void CodeGenerator::emit_print_struct_inline(llvm::Value* val, const std::string& type_name) {
    auto st = llvm::cast<llvm::StructType>(val->getType());
    int n = (int)st->getNumElements();
    llvm::FunctionType* printf_ft = llvm::FunctionType::get(builder->getInt32Ty(), llvm::PointerType::getUnqual(*context), true);
    auto printf_func = module->getOrInsertFunction("printf", printf_ft);
    auto tmp = builder->CreateAlloca(st, nullptr, "print.struct");
    builder->CreateStore(val, tmp);
    auto open_br = builder->CreateGlobalString("{", "open_br");
    builder->CreateCall(printf_func, {open_br});
    (void)type_name;
    for (int i = 0; i < n; ++i) {
        auto eptr = builder->CreateInBoundsGEP(st, tmp, {builder->getInt32(0), builder->getInt32(i)}, "print.field");
        auto ev = builder->CreateLoad(st->getElementType(i), eptr, "print.val");
        if (i > 0) {
            auto comma = builder->CreateGlobalString(", ", "comma");
            builder->CreateCall(printf_func, {comma});
        }
        if (is_string_slice(ev->getType())) {
            emit_print_scalar(ev, "string");
        }
        else if (is_slice_ty(ev->getType())) {
            emit_slice_print(ev, false);
        }
        else if (ev->getType()->isStructTy()) {
            emit_print_struct_inline(ev, lltype_to_name(ev->getType()));
        }
        else {
            emit_print_scalar(ev, lltype_to_name(ev->getType()));
        }
    }
    auto close_br = builder->CreateGlobalString("}", "close_br");
    builder->CreateCall(printf_func, {close_br});
}

void CodeGenerator::generate_block(const std::shared_ptr<Block>& block) {
    if (!block) return;
    push_scope();
    for (const auto& stmt : block->body) generate_stmt(stmt);
    pop_scope();
}

void CodeGenerator::generate_if(const If& if_stmt) {
    auto cond = generate_expr(if_stmt.condition);
    if (!cond) return;
    require_bool(cond, "if");

    auto current = builder->GetInsertBlock();
    if (current->getTerminatorOrNull()) return;

    llvm::Function* func = current->getParent();
    auto then_block = llvm::BasicBlock::Create(*context, "then", func);
    auto else_block = llvm::BasicBlock::Create(*context, "else", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "merge");

    builder->CreateCondBr(cond, then_block, else_block);
    builder->SetInsertPoint(then_block);
    generate_block(if_stmt.then_block);
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(merge_block);

    builder->SetInsertPoint(else_block);
    generate_block(if_stmt.else_block);
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(merge_block);

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
}

void CodeGenerator::generate_while(const While& w) {
    auto func = builder->GetInsertBlock()->getParent();
    auto cond_block = llvm::BasicBlock::Create(*context, "while.cond", func);
    auto body_block = llvm::BasicBlock::Create(*context, "while.body", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "while.end");

    builder->CreateBr(cond_block);
    builder->SetInsertPoint(cond_block);
    auto cond = generate_expr(w.condition);
    if (!cond) return;
    require_bool(cond, "while");
    builder->CreateCondBr(cond, body_block, merge_block);

    builder->SetInsertPoint(body_block);
    loop_ctx.push_back({merge_block, cond_block});
    generate_block(w.body);
    loop_ctx.pop_back();
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(cond_block);

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
}

void CodeGenerator::generate_for(const For& f) {
    auto func = builder->GetInsertBlock()->getParent();
    if (f.init) generate_stmt(f.init);

    auto cond_block = llvm::BasicBlock::Create(*context, "for.cond", func);
    auto body_block = llvm::BasicBlock::Create(*context, "for.body", func);
    auto step_block = llvm::BasicBlock::Create(*context, "for.step", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "for.end");

    builder->CreateBr(cond_block);
    builder->SetInsertPoint(cond_block);
    if (f.condition) {
        auto cond = generate_expr(f.condition);
        if (!cond) return;
        require_bool(cond, "for condition");
        builder->CreateCondBr(cond, body_block, merge_block);
    } else {
        builder->CreateBr(body_block);
    }

    builder->SetInsertPoint(body_block);
    loop_ctx.push_back({merge_block, step_block});
    generate_block(f.body);
    loop_ctx.pop_back();
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(step_block);

    builder->SetInsertPoint(step_block);
    if (f.step) generate_stmt(f.step);
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(cond_block);

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
}

void CodeGenerator::generate_for_in(const ForIn& fi) {
    auto func = builder->GetInsertBlock()->getParent();
    llvm::Value* arr = generate_expr(fi.iterable);
    if (!arr) return;

    llvm::Type* elem = nullptr;
    llvm::Value* data_ptr = nullptr;
    llvm::Value* len_val = nullptr;
    bool is_slice = is_slice_ty(arr->getType());
    if (is_slice) {
        elem = slice_elem_type(llvm::cast<llvm::StructType>(arr->getType()));
        data_ptr = builder->CreateExtractValue(arr, 0, "forin.ptr");
        len_val = builder->CreateExtractValue(arr, 1, "forin.len");
    } else {
        auto at = llvm::dyn_cast<llvm::ArrayType>(arr->getType());
        if (!at) error("for-in requires an array, got " + llvm_type_name(arr->getType()));
        elem = at->getElementType();
    }
    int N = is_slice ? -1 : (int)llvm::cast<llvm::ArrayType>(arr->getType())->getNumElements();

    push_scope();
    auto xalloc = builder->CreateAlloca(elem, nullptr, fi.var);
    define_var(fi.var, xalloc, elem);

    auto i = builder->CreateAlloca(builder->getInt32Ty(), nullptr, "forin.i");
    builder->CreateStore(builder->getInt32(0), i);

    auto cond_block = llvm::BasicBlock::Create(*context, "forin.cond", func);
    auto body_block = llvm::BasicBlock::Create(*context, "forin.body", func);
    auto step_block = llvm::BasicBlock::Create(*context, "forin.step", func);
    auto merge_block = llvm::BasicBlock::Create(*context, "forin.end");

    builder->CreateBr(cond_block);
    builder->SetInsertPoint(cond_block);
    auto icur = builder->CreateLoad(builder->getInt32Ty(), i, "forin.i.cur");
    llvm::Value* cmp;
    if (is_slice) {
        cmp = builder->CreateICmpSLT(icur, len_val, "forin.cmp");
    } else {
        cmp = builder->CreateICmpSLT(icur, builder->getInt32(N), "forin.cmp");
    }
    builder->CreateCondBr(cmp, body_block, merge_block);

    builder->SetInsertPoint(body_block);
    llvm::Value* eptr;
    if (is_slice) {
        eptr = builder->CreateInBoundsGEP(elem, data_ptr, {icur}, "forin.elem");
    } else {
        auto tmp = builder->CreateAlloca(arr->getType(), nullptr, "forin.arr");
        builder->CreateStore(arr, tmp);
        eptr = builder->CreateInBoundsGEP(arr->getType(), tmp, {builder->getInt32(0), icur}, "forin.elem");
    }
    auto ev = builder->CreateLoad(elem, eptr, "forin.val");
    builder->CreateStore(ev, xalloc);
    loop_ctx.push_back({merge_block, step_block});
    generate_block(fi.body);
    loop_ctx.pop_back();
    if (!block_has_terminator(builder->GetInsertBlock())) builder->CreateBr(step_block);

    builder->SetInsertPoint(step_block);
    auto inext = builder->CreateAdd(icur, builder->getInt32(1), "forin.i.next");
    builder->CreateStore(inext, i);
    builder->CreateBr(cond_block);

    func->insert(func->end(), merge_block);
    builder->SetInsertPoint(merge_block);
    pop_scope();
}

void CodeGenerator::generate_break(const Break& b) {
    if (loop_ctx.empty()) {
        error("Cannot use 'break' outside a loop");
    }
    builder->CreateBr(loop_ctx.back().first);
}

void CodeGenerator::generate_continue(const Continue& c) {
    if (loop_ctx.empty()) {
        error("Cannot use 'continue' outside a loop");
    }
    builder->CreateBr(loop_ctx.back().second);
}

void CodeGenerator::generate_return(const Return& r) {
    if (epoch_depth > 0) {
        error("Cannot return from inside an epoch block: the arena rollback would invalidate any data you sent back");
    }
    llvm::Type* ret_ty = return_type_stack.empty() ? nullptr : return_type_stack.back();
    std::string ret_name = return_name_stack.empty() ? "" : return_name_stack.back();
    if (r.value) {
        llvm::Value* val = (!ret_name.empty() && ret_ty)
            ? coerce_expr_to(r.value, ret_name)
            : generate_expr(r.value);
        if (!val) return;
        if (ret_ty && is_slice_ty(ret_ty)) {
            // Returning a fixed (stack) array from an int[] function: copy the
            // values into fresh arena memory so the returned slice owns its data.
            if (val->getType()->isArrayTy()) {
                val = copy_array_to_slice(val, llvm::cast<llvm::StructType>(ret_ty));
            } else if (val->getType() != ret_ty) {
                error("Cannot return " + llvm_type_name(val->getType()) + " where " + llvm_type_name(ret_ty) + " is expected");
            }
        } else if (ret_ty && ret_ty->isDoubleTy() && val->getType()->isIntegerTy(32)) {
            val = builder->CreateSIToFP(val, builder->getDoubleTy());
        }
        builder->CreateRet(val);
    } else {
        if (ret_ty && !ret_ty->isVoidTy()) {
            if (ret_ty->isIntegerTy(32)) builder->CreateRet(builder->getInt32(0));
            else if (ret_ty->isDoubleTy()) builder->CreateRet(llvm::ConstantFP::get(builder->getDoubleTy(), 0.0));
            else if (ret_ty->isIntegerTy(1)) builder->CreateRet(builder->getInt1(false));
            else if (ret_ty->isArrayTy()) builder->CreateRet(llvm::ConstantAggregateZero::get(ret_ty));
            else if (ret_ty->isStructTy()) builder->CreateRet(llvm::ConstantAggregateZero::get(ret_ty));
            else builder->CreateRet(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ret_ty)));
        } else {
            builder->CreateRetVoid();
        }
    }
}

// ============================================================================
// Function Declaration & Body Generation
// ============================================================================

void CodeGenerator::declare_func(const FuncDecl& fd) {
    std::vector<llvm::Type*> param_types;
    for (auto& p : fd.params) {
        std::string pbase;
        int parr = -1;
        parse_type(p.var_type, pbase, parr);
        if (p.is_ref) {
            // ref parameter: the caller passes the address of the lvalue.
            param_types.push_back(builder->getPtrTy());
        } else if (parr == 0) {
            // Open array (int[]) is a single slice { base*, i32 } argument
            param_types.push_back(llvm_type_from_name(p.var_type));
        } else {
            llvm::Type* t = signature_type_for(p.var_type, "parameter '" + p.name + "'");
            if (!t) t = builder->getInt32Ty();
            param_types.push_back(t);
        }
    }
    llvm::Type* ret_ty = signature_type_for(fd.return_type, "return type of '" + fd.name + "'");
    if (!ret_ty) ret_ty = builder->getVoidTy();

    auto func_type = llvm::FunctionType::get(ret_ty, param_types, false);
    auto func = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage, fd.name, module.get());
    functions[fd.name] = func;
    func_params_[fd.name] = fd.params;
    return_type_names_[fd.name] = fd.return_type;
}

void CodeGenerator::generate_func_decl(const FuncDecl& fd) {
    auto it = functions.find(fd.name);
    if (it == functions.end()) return;
    llvm::Function* func = it->second;

    auto entry = llvm::BasicBlock::Create(*context, "entry", func);
    builder->SetInsertPoint(entry);

    push_scope();
    return_type_stack.push_back(func->getReturnType()->isVoidTy() ? nullptr : func->getReturnType());
    return_name_stack.push_back(fd.return_type);

    auto arg_it = func->arg_begin();
    for (auto& p : fd.params) {
        std::string pbase;
        int parr = -1;
        parse_type(p.var_type, pbase, parr);
        if (p.is_ref) {
            // ref parameter: the incoming value is the caller's address.
            // Store it in a local slot and remember that reads/writes go
            // through one extra pointer layer (ref_indirect via ref_scopes_).
            llvm::Argument& arg = *arg_it++;
            llvm::Type* val_ty = signature_type_for(p.var_type, "parameter '" + p.name + "'");
            if (!val_ty) val_ty = builder->getInt32Ty();
            auto slot = builder->CreateAlloca(builder->getPtrTy(), nullptr, p.name);
            builder->CreateStore(&arg, slot);
            named_values.back()[p.name] = slot;
            named_types.back()[p.name] = val_ty;
            ref_scopes_.back().insert(p.name);
            var_type_names_.back()[p.name] = p.var_type;
        } else if (parr == 0) {
            // Open array: one slice { base*, i32 } argument, stored as a whole
            llvm::Argument& arg = *arg_it++;
            llvm::Type* st = llvm_type_from_name(p.var_type);
            auto alloca = builder->CreateAlloca(st, nullptr, p.name);
            builder->CreateStore(&arg, alloca);
            named_values.back()[p.name] = alloca;
            named_types.back()[p.name] = st;
            var_type_names_.back()[p.name] = p.var_type;
            register_open_array(p.name, llvm_type_from_name(pbase));
            open_epochs[p.name] = epoch_depth;
        } else {
            llvm::Argument& arg = *arg_it++;
            llvm::Type* pt = signature_type_for(p.var_type, "parameter '" + p.name + "'");
            if (!pt) pt = builder->getInt32Ty();
            auto alloca = builder->CreateAlloca(pt, nullptr, p.name);
            builder->CreateStore(&arg, alloca);
            define_var(p.name, alloca, pt, p.var_type);
        }
    }

    generate_block(fd.body);
    if (!block_has_terminator(builder->GetInsertBlock())) {
        llvm::Type* ret_ty = func->getReturnType();
        if (ret_ty->isVoidTy()) builder->CreateRetVoid();
        else if (ret_ty->isIntegerTy(32)) builder->CreateRet(builder->getInt32(0));
        else if (ret_ty->isDoubleTy()) builder->CreateRet(llvm::ConstantFP::get(builder->getDoubleTy(), 0.0));
        else if (ret_ty->isIntegerTy(1)) builder->CreateRet(builder->getInt1(false));
        else if (ret_ty->isArrayTy()) builder->CreateRet(llvm::ConstantAggregateZero::get(ret_ty));
        else if (ret_ty->isStructTy()) builder->CreateRet(llvm::ConstantAggregateZero::get(ret_ty));
        else builder->CreateRet(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ret_ty)));
    }

    return_type_stack.pop_back();
    return_name_stack.pop_back();
    pop_scope();
}

void CodeGenerator::generate_stmt(const std::shared_ptr<Stmt>& stmt) {
    if (!stmt) return;
    // Skip statements after a terminator (return/break/continue): the block is
    // already closed and any instruction emitted into it would be invalid IR.
    auto cur = builder->GetInsertBlock();
    if (cur && cur->getTerminatorOrNull()) return;
    if (stmt->line > 0) {
        current_line = stmt->line;
        current_col = stmt->col;
    }
    switch (stmt->kind) {
        case Stmt::Kind::Let:      generate_let(stmt->let); break;
        case Stmt::Kind::Hot:      generate_hot(stmt->hot); break;
        case Stmt::Kind::Epoch:    generate_epoch(stmt->epoch); break;
        case Stmt::Kind::Print:    generate_print(stmt->print); break;
        case Stmt::Kind::If:       if (stmt->if_stmt) generate_if(*stmt->if_stmt); break;
        case Stmt::Kind::While:    if (stmt->while_stmt) generate_while(*stmt->while_stmt); break;
        case Stmt::Kind::For:      if (stmt->for_stmt) generate_for(*stmt->for_stmt); break;
        case Stmt::Kind::ForIn:    if (stmt->for_in_stmt) generate_for_in(*stmt->for_in_stmt); break;
        case Stmt::Kind::Return:   if (stmt->return_stmt) generate_return(*stmt->return_stmt); break;
        case Stmt::Kind::Assign:   if (stmt->assign_stmt) generate_assign(*stmt->assign_stmt); break;
        case Stmt::Kind::FuncDecl: generate_func_decl(*stmt->func_decl); break;
        case Stmt::Kind::CallStmt: if (stmt->call_stmt) generate_expr(stmt->call_stmt); break;
        case Stmt::Kind::StructDecl: break; // already registered in pass 1
        case Stmt::Kind::EnumDecl:   break; // already registered in pass 1
        case Stmt::Kind::Break:    generate_break(stmt->break_stmt); break;
        case Stmt::Kind::Continue: generate_continue(stmt->continue_stmt); break;
    }
}

bool CodeGenerator::block_has_terminator(llvm::BasicBlock* bb) {
    if (bb->empty()) return false;
    return bb->back().isTerminator();
}

void CodeGenerator::finalize() {
    auto cur = builder->GetInsertBlock();
    if (cur && !cur->getTerminatorOrNull()) {
        builder->CreateRet(builder->getInt32(0));
    }
    llvm::verifyFunction(*main_func, &llvm::errs());
    module->print(llvm::outs(), nullptr);
}

void CodeGenerator::generate_program(const Program& program) {
    // Pass 1: register struct types and enum names (needed before function signatures)
    for (const auto& stmt : program.body) {
        if (stmt->kind == Stmt::Kind::StructDecl && stmt->struct_decl) register_struct(*stmt->struct_decl);
    }
    for (const auto& stmt : program.body) {
        if (stmt->kind == Stmt::Kind::EnumDecl && stmt->enum_decl) register_enum(*stmt->enum_decl);
    }
    for (const auto& stmt : program.body) {
        if (stmt->kind == Stmt::Kind::FuncDecl && stmt->func_decl) declare_func(*stmt->func_decl);
    }
    for (const auto& stmt : program.body) {
        if (stmt->kind == Stmt::Kind::FuncDecl && stmt->func_decl) generate_func_decl(*stmt->func_decl);
    }
    builder->SetInsertPoint(main_entry_block);
    // Persist argc/argv for argc()/arg() anywhere in the program.
    builder->CreateStore(main_func->getArg(0), argc_global);
    builder->CreateStore(main_func->getArg(1), argv_global);
    for (const auto& stmt : program.body) {
        if (stmt->kind != Stmt::Kind::FuncDecl && stmt->kind != Stmt::Kind::StructDecl) generate_stmt(stmt);
    }
}
