import sys
import json
from .parser import parse
from .ast import to_dict

def main():
    if len(sys.argv) < 2:
        print("Usage: python -m src.main <file.aev>")
        sys.exit(1)

    filename = sys.argv[1]
    with open(filename, "r") as f:
        code = f.read()

    try:
        ast = parse(code)
    except Exception as e:
        print(f"❌ Parse error: {e}")
        sys.exit(1)

    json_data = to_dict(ast)

    with open("ast.json", "w") as f:
        json.dump(json_data, f, indent=2)

    print(f"✅ AST saved to ast.json")

if __name__ == "__main__":
    main()
