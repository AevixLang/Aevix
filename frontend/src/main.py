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
    the resulting AST to ast.json.
    """
    if len(sys.argv) < 2:
        print("Usage: python -m src.main <file.aev>", file=sys.stderr)
        sys.exit(1)

    filename = sys.argv[1]
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

    with open("ast.json", "w") as f:
        json.dump(json_data, f, indent=2)

    print(f"✅ AST saved to ast.json")


if __name__ == "__main__":
    main()