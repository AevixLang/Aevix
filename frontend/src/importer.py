# ============================================================================
# Aevix Frontend: Import Resolver
#
# Implements Stage 3 (import) semantics. Recursively resolves `import "spec";`
# statements on the top level of a .aev file and merges the declarations of
# all imported modules into one flat Program, which the backend already knows
# how to consume.
#
# The connection to the future package manager ("aevix-pm", a separate repo)
# is by contract: the compiler is dumb (read-only) and the PM is smart.
# See notes/package-manager.md for the full contract.
# ============================================================================
import os
import sys
import json

from .parser import parse, format_parse_error
from .ast import (
    Program, Import, FuncDecl, StructDecl, EnumDecl, Param, StructField,
    Let, Hot, Epoch, Print, If, While, For, ForIn, Return, Assign, Match,
    Call, Variable, MemberAccess, StructLiteral, New, Index, ArrayLit,
    Neg, Not, Add, Sub, Mul, Div, Mod, CmpOp, And, Or,
)

PACKAGE_ROOT_ENV = "AEVIX_PATH"
GLOBAL_ROOT = os.path.expanduser(os.path.join("~", ".aevix"))


class ImportResolveError(Exception):
    """A user-facing error in import resolution (missing module, cycle, ...)."""


class SourceParseError(Exception):
    """A parse failure in the main file or in an imported module.
    Carries the offending source so the caller can render a good message."""

    def __init__(self, path, code, cause):
        super().__init__(str(cause))
        self.path = path
        self.code = code
        self.cause = cause


def _display(path):
    """Short name for error messages."""
    return os.path.basename(path)


def _candidate(base, rel):
    """Returns an existing file for `base` + `rel`, or None.
    Tries: exact file, file + '.aev' (if rel has no extension), or
    a directory whose entry point is lib.aev."""
    p = os.path.join(base, rel)
    if os.path.isfile(p):
        return p
    if not rel.endswith(".aev") and os.path.isfile(p + ".aev"):
        return p + ".aev"
    if os.path.isdir(p) and os.path.isfile(os.path.join(p, "lib.aev")):
        return os.path.join(p, "lib.aev")
    return None


def _find_project_root(start):
    """Nearest directory above `start` that contains aevix.toml or aevix.lock.
    Falls back to `start` itself."""
    d = os.path.abspath(start)
    while True:
        if os.path.isfile(os.path.join(d, "aevix.toml")) or os.path.isfile(
            os.path.join(d, "aevix.lock")
        ):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return os.path.abspath(start)
        d = parent


def _find_lock(start):
    """Walks up from `start` looking for aevix.lock.
    Returns (lock_dir, parsed_json) or (None, None)."""
    d = os.path.abspath(start)
    while True:
        p = os.path.join(d, "aevix.lock")
        if os.path.isfile(p):
            try:
                with open(p, "r") as f:
                    data = json.load(f)
            except (OSError, ValueError) as e:
                raise ImportResolveError(f"cannot read {p}: {e}")
            return os.path.dirname(os.path.realpath(p)), data
        parent = os.path.dirname(d)
        if parent == d:
            return None, None
        d = parent


def _resolve_path(spec, from_dir, importer):
    """Resolves a plain path spec: relative file → project lib/ → AEVIX_PATH."""
    from_dir = os.path.abspath(from_dir)
    project = _find_project_root(from_dir)
    bases = [from_dir, os.path.join(project, "lib")]
    for entry in os.environ.get(PACKAGE_ROOT_ENV, "").split(os.pathsep):
        if entry:
            bases.append(entry)

    seen, uniq = set(), []
    for b in bases:
        rb = os.path.realpath(b)
        if rb not in seen:
            seen.add(rb)
            uniq.append(b)

    for base in uniq:
        cand = _candidate(base, spec)
        if cand:
            return os.path.realpath(cand)

    searched = [os.path.join(b, spec) for b in uniq]
    raise ImportResolveError(
        f"module '{spec}' not found (imported from {_display(importer)}); "
        f"searched: {', '.join(searched)}"
    )


def _resolve_package(spec, from_dir, importer):
    """Resolves 'name:module' against aevix.lock (written by the package manager)."""
    name, module = spec.split(":", 1)
    lockdir, data = _find_lock(from_dir)
    if data is None:
        raise ImportResolveError(
            f"package '{name}' not found (imported from {_display(importer)}): "
            "project has no aevix.lock; initialize it with the package manager "
            "(e.g. 'aevix-pm init')"
        )
    dep = (data.get("dependencies") or {}).get(name)
    if dep is None:
        raise ImportResolveError(
            f"package '{name}' is not in aevix.lock (imported from "
            f"{_display(importer)}); add it with the package manager "
            f"(e.g. 'aevix-pm add {name}')"
        )
    if dep.get("path"):
        pkgdir = dep["path"]
        if not os.path.isabs(pkgdir):
            pkgdir = os.path.join(lockdir, pkgdir)
    else:
        version = dep.get("version")
        if not version:
            raise ImportResolveError(
                f"package '{name}' in aevix.lock has no version or path"
            )
        pkgdir = os.path.join(GLOBAL_ROOT, "pkg", f"{name}@{version}")
    cand = _candidate(pkgdir, module)
    if cand is None:
        raise ImportResolveError(
            f"module '{module}' not found in package '{name}' "
            f"(imported from {_display(importer)}; pkg dir: {pkgdir})"
        )
    return os.path.realpath(cand)


def _resolve_import(spec, from_dir, importer):
    spec = spec.strip()
    if spec.startswith("std:"):
        module = spec[4:]
        stdlib_dir = _find_stdlib_dir(from_dir)
        if stdlib_dir:
            cand = _candidate(stdlib_dir, module)
            if cand:
                return cand
        raise ImportResolveError(
            f"standard library module 'std:{module}' not found (imported from "
            f"{_display(importer)}; searched stdlib relative to compiler)")
    if ":" in spec:
        return _resolve_package(spec, from_dir, importer)
    return _resolve_path(spec, from_dir, importer)


def _find_stdlib_dir(start):
    """Walk up from start looking for a stdlib/ directory."""
    import os
    d = os.path.abspath(start)
    while d:
        cand = os.path.join(d, "stdlib")
        if os.path.isdir(cand):
            return cand
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return None


def _statement_bodies(s):
    """Yields the statement-list fields of a statement (func/hot/if/while/for bodies)."""
    for field in ("body", "then_body", "else_body"):
        child = getattr(s, field, None)
        if isinstance(child, list):
            yield child


def _find_nested_import(program):
    """Returns the first Import that is NOT on the top level of the file."""
    def scan(body):
        for s in body:
            if isinstance(s, Import):
                return s
            for child in _statement_bodies(s):
                found = scan(child)
                if found:
                    return found
        return None
    for s in program.body:
        for child in _statement_bodies(s):
            found = scan(child)
            if found:
                return found
    return None


def _parse_file(path):
    try:
        with open(path, "r") as f:
            code = f.read()
    except OSError as e:
        raise ImportResolveError(f"Cannot open file: {e}")
    try:
        return parse(code)
    except Exception as e:
        raise SourceParseError(path, code, e)


def _rename_type(fn, name):
    """Applies `fn` to a type name's base, keeping any array suffix."""
    if not name:
        return name
    bracket = name.find("[")
    base = name if bracket < 0 else name[:bracket]
    rest = "" if bracket < 0 else name[bracket:]
    mapped = fn(base)
    return mapped + rest if mapped else name


def _rename_expr(e, fn):
    """Rewrites declaration references inside an expression in place.
    Returns the (possibly replaced) node."""
    if isinstance(e, MemberAccess):
        if isinstance(e.object, Variable):
            qualified = e.object.value + "." + e.member
            mapped = fn(qualified)
            if mapped:
                return Variable(value=mapped)
            base = fn(e.object.value)
            if base:
                e.object.value = base
            return e
        e.object = _rename_expr(e.object, fn)
    elif isinstance(e, Call):
        mapped = fn(e.callee)
        if mapped:
            e.callee = mapped
        e.args = [_rename_expr(a, fn) for a in e.args]
    elif isinstance(e, ArrayLit):
        e.elements = [_rename_expr(x, fn) for x in e.elements]
    elif isinstance(e, New):
        e.base = _rename_type(fn, e.base)
        e.size = _rename_expr(e.size, fn)
    elif isinstance(e, Index):
        e.object = _rename_expr(e.object, fn)
        e.index = _rename_expr(e.index, fn)
    elif isinstance(e, StructLiteral):
        mapped = fn(e.name)
        if mapped:
            e.name = mapped
        e.args = [_rename_expr(a, fn) for a in e.args]
    elif isinstance(e, (Neg, Not)):
        e.value = _rename_expr(e.value, fn)
    elif isinstance(e, (Add, Sub, Mul, Div, Mod, CmpOp, And, Or)):
        e.left = _rename_expr(e.left, fn)
        e.right = _rename_expr(e.right, fn)
    return e


def _rename_stmt(s, fn):
    """Rewrites declaration references inside a statement in place.
    Returns the (possibly replaced) node."""
    if isinstance(s, Let):
        s.value = _rename_expr(s.value, fn)
        s.var_type = _rename_type(fn, s.var_type)
    elif isinstance(s, (Hot, Epoch)):
        s.body = [_rename_stmt(x, fn) for x in s.body]
    elif isinstance(s, Print):
        s.args = [_rename_expr(a, fn) for a in s.args]
    elif isinstance(s, If):
        s.condition = _rename_expr(s.condition, fn)
        s.then_body = [_rename_stmt(x, fn) for x in s.then_body]
        if s.else_body is not None:
            s.else_body = [_rename_stmt(x, fn) for x in s.else_body]
    elif isinstance(s, While):
        s.condition = _rename_expr(s.condition, fn)
        s.body = [_rename_stmt(x, fn) for x in s.body]
    elif isinstance(s, For):
        if s.init is not None:
            s.init = _rename_stmt(s.init, fn)
        if s.condition is not None:
            s.condition = _rename_expr(s.condition, fn)
        if s.step is not None:
            s.step = _rename_stmt(s.step, fn)
        s.body = [_rename_stmt(x, fn) for x in s.body]
    elif isinstance(s, ForIn):
        s.iterable = _rename_expr(s.iterable, fn)
        s.body = [_rename_stmt(x, fn) for x in s.body]
    elif isinstance(s, Return):
        if s.value is not None:
            s.value = _rename_expr(s.value, fn)
    elif isinstance(s, Assign):
        s.name = _rename_expr(s.name, fn)
        s.value = _rename_expr(s.value, fn)
    elif isinstance(s, Match):
        s.scrutinee = _rename_expr(s.scrutinee, fn)
        for c in s.cases:
            c.value = _rename_expr(c.value, fn)
            c.body = [_rename_stmt(x, fn) for x in c.body]
        if s.default_body is not None:
            s.default_body = [_rename_stmt(x, fn) for x in s.default_body]
    elif isinstance(s, FuncDecl):
        mapped = fn(s.name)
        if mapped:
            s.name = mapped
        for p in s.params:
            p.var_type = _rename_type(fn, p.var_type)
        s.return_type = _rename_type(fn, s.return_type)
        s.body = [_rename_stmt(x, fn) for x in s.body]
    elif isinstance(s, StructDecl):
        mapped = fn(s.name)
        if mapped:
            s.name = mapped
        for fld in s.fields:
            fld.var_type = _rename_type(fn, fld.var_type)
    elif isinstance(s, EnumDecl):
        mapped = fn(s.name)
        if mapped:
            s.name = mapped
    else:
        return _rename_expr(s, fn)
    return s


def _prefix_module(decls, alias):
    """Renames a module's exported declarations to `<alias>__<name>` and rewrites
    the module's internal references. Returns original -> prefixed."""
    exported = {}
    for s in decls:
        if isinstance(s, (FuncDecl, StructDecl, EnumDecl)):
            exported[s.name] = alias + "__" + s.name
    fn = lambda n: exported.get(n, "")
    decls[:] = [_rename_stmt(s, fn) for s in decls]
    return exported


def _rewrite_qualified(stmts, nsmap):
    """Resolves `alias.name` references in the importing module."""
    if not nsmap:
        return
    def fn(name):
        if "." not in name:
            return ""
        alias, rest = name.split(".", 1)
        return nsmap.get(alias, {}).get(rest, "")
    stmts[:] = [_rename_stmt(s, fn) for s in stmts]


def _check_conflict(s, file, origins, checked):
    # Nested levels hand the same node to every caller; validate it once.
    if id(s) in checked:
        return
    checked.add(id(s))
    prev = origins.get(s.name)
    if prev is not None and prev[1] != file:
        raise ImportResolveError(
            f"name conflict: '{s.name}' is defined in both "
            f"'{_display(prev[1])}' and '{_display(file)}'"
        )
    origins[s.name] = (s.type, file)


def _visit(path, stack, loaded, origins, checked, is_main=False):
    """Returns the flattened, internally consistent statement list of `path`.
    `is_main` keeps top-level code; imported modules contribute declarations."""
    key = os.path.realpath(os.path.abspath(path))
    if key in stack:
        chain = stack[stack.index(key):] + [key]
        raise ImportResolveError(
            "circular import detected: " + " -> ".join(_display(p) for p in chain)
        )
    program = _parse_file(key)
    nested = _find_nested_import(program)
    if nested is not None:
        raise ImportResolveError(
            f"import is only allowed at the top level of a file: '{_display(key)}'"
        )
    stack.append(key)
    own, nsmap = [], {}
    for s in program.body:
        if isinstance(s, Import):
            sub = _resolve_import(s.spec, os.path.dirname(key), key)
            sub_key = os.path.realpath(os.path.abspath(sub))
            if not s.alias:
                if sub_key in loaded:
                    continue
                decls = _visit(sub, stack, loaded, origins, checked, False)
                loaded.add(sub_key)
                for d in decls:
                    _check_conflict(d, sub_key, origins, checked)
                    own.append(d)
            else:
                decls = _visit(sub, stack, loaded, origins, checked, False)
                nsmap[s.alias] = _prefix_module(decls, s.alias)
                for d in decls:
                    _check_conflict(d, sub_key, origins, checked)
                    own.append(d)
        elif isinstance(s, (FuncDecl, StructDecl, EnumDecl)):
            # Imported declarations are checked by the caller after prefixing.
            if is_main:
                _check_conflict(s, key, origins, checked)
            own.append(s)
        elif is_main:
            own.append(s)  # the main file keeps its top-level statements
    stack.pop()
    _rewrite_qualified(own, nsmap)
    return own


def load_program(path):
    """Parses `path`, recursively resolves its top-level imports (honouring
    `import ... as alias`), and returns one flat Program."""
    main_path = os.path.realpath(os.path.abspath(path))
    stack = []
    loaded = set()
    origins = {}
    checked = set()
    out = _visit(main_path, stack, loaded, origins, checked, True)
    return Program(body=out)