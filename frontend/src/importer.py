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
from .ast import Program, Import, FuncDecl, StructDecl

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
        raise ImportResolveError(
            f"'std:' is reserved for the standard library, which is not "
            f"available yet (imported from {_display(importer)})"
        )
    if ":" in spec:
        return _resolve_package(spec, from_dir, importer)
    return _resolve_path(spec, from_dir, importer)


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


def _visit(path, stack, loaded, out, origins):
    """Loads `path` and appends its declarations (and those of its imports,
    depth-first, deduplicated) to `out`. `origins` maps name -> (kind, file)
    for cross-file conflict detection."""
    key = os.path.realpath(os.path.abspath(path))
    if key in stack:
        chain = stack[stack.index(key):] + [key]
        raise ImportResolveError(
            "circular import detected: " + " -> ".join(_display(p) for p in chain)
        )
    if key in loaded:
        return
    program = _parse_file(key)
    nested = _find_nested_import(program)
    if nested is not None:
        raise ImportResolveError(
            f"import is only allowed at the top level of a file: '{_display(key)}'"
        )
    stack.append(key)
    for s in program.body:
        if isinstance(s, Import):
            sub = _resolve_import(s.spec, os.path.dirname(key), key)
            _visit(sub, stack, loaded, out, origins)
        elif isinstance(s, (FuncDecl, StructDecl)):
            prev = origins.get(s.name)
            if prev is not None and prev[1] != key:
                raise ImportResolveError(
                    f"name conflict: '{s.name}' is defined in both "
                    f"'{_display(prev[1])}' and '{_display(key)}'"
                )
            origins[s.name] = (s.type, key)
            out.append(s)
        # top-level statements of an imported module are dropped: modules
        # must not execute side effects just by being imported.
    stack.pop()
    loaded.add(key)


def load_program(path):
    """Parses `path`, recursively resolves its top-level imports, and returns
    a single flat Program containing the merged declarations."""
    main_path = os.path.realpath(os.path.abspath(path))
    program = _parse_file(main_path)
    nested = _find_nested_import(program)
    if nested is not None:
        raise ImportResolveError(
            f"import is only allowed at the top level of a file: "
            f"'{_display(main_path)}'"
        )

    stack = []
    loaded = set()
    origins = {}
    out = []
    for s in program.body:
        if isinstance(s, Import):
            sub = _resolve_import(s.spec, os.path.dirname(main_path), main_path)
            _visit(sub, stack, loaded, out, origins)
        elif isinstance(s, (FuncDecl, StructDecl)):
            prev = origins.get(s.name)
            if prev is not None and prev[1] != main_path:
                raise ImportResolveError(
                    f"name conflict: '{s.name}' is defined in both "
                    f"'{_display(prev[1])}' and '{_display(main_path)}'"
                )
            origins[s.name] = (s.type, main_path)
            out.append(s)
        else:
            out.append(s)  # the main file keeps its top-level statements

    program.body = out
    return program