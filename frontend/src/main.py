import sys
import json
from .parser import parse, format_parse_error
from .ast import to_dict

def main():
    if len(sys.argv) < 2:
        print("Usage: python -m src.main <file.aev>", file=sys.stderr)
        sys.exit(1)

    filename = sys.argv[1]
    try:
        with open(filename, "r") as f:
            code = f.read()
    except OSError as e:
        print(f"❌ Cannot open file: {e}", file=sys.stderr)
        sys.exit(1)

    try:
        ast = parse(code)
    except Exception as e:
        print(f"❌ Parse error in {filename}:", file=sys.stderr)
        print(format_parse_error(code, e), file=sys.stderr)
        sys.exit(1)

    json_data = to_dict(ast)

    with open("ast.json", "w") as f:
        json.dump(json_data, f, indent=2)

    print(f"✅ AST saved to ast.json")

if __name__ == "__main__":
    main()
