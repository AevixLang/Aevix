# ============================================================================
# Aevix Frontend: Main Entry Point
#
# This module is the primary entry point for the frontend. It coordinates
# the reading, import resolution, parsing of .aev source files, AST
# transformation, and JSON export.
# ============================================================================
import sys
import json
from .parser import format_parse_error
from .importer import load_program, ImportResolveError, SourceParseError
from .ast import to_dict


def main():
    """
    Parses the provided Aevix source file (resolving its imports) and saves
    the resulting AST to a JSON file.

    Usage: python -m src.main <file.aev> [-o <output.json>]
    """
    args = sys.argv[1:]
    out_path = "ast.json"

    # Simple flag parsing: -o <path> for output location.
    i = 0
    positional = []
    while i < len(args):
        if args[i] == "-o" and i + 1 < len(args):
            out_path = args[i + 1]
            i += 2
        elif args[i].startswith("-"):
            print(f"Unknown flag: {args[i]}", file=sys.stderr)
            sys.exit(1)
        else:
            positional.append(args[i])
            i += 1

    if len(positional) < 1:
        print("Usage: python -m src.main <file.aev> [-o <output.json>]", file=sys.stderr)
        sys.exit(1)

    filename = positional[0]
    try:
        program = load_program(filename)
    except SourceParseError as e:
        print(f"❌ Parse error in {e.path}:", file=sys.stderr)
        print(format_parse_error(e.code, e.cause), file=sys.stderr)
        sys.exit(1)
    except ImportResolveError as e:
        print(f"❌ {e}", file=sys.stderr)
        sys.exit(1)

    # Serialize AST to JSON for the backend
    json_data = to_dict(program)

    with open(out_path, "w") as f:
        json.dump(json_data, f, indent=2)

    print(f"✅ AST saved to {out_path}")


if __name__ == "__main__":
    main()