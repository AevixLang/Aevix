import os
from lark import Lark, UnexpectedInput
from .transformer import AevixTransformer

def get_grammar_path():
    current_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(current_dir, "..", "grammar", "aevix.lark")

def format_parse_error(code: str, exc: Exception) -> str:
    if not isinstance(exc, UnexpectedInput):
        return str(exc)

    pos = exc.pos_in_stream if hasattr(exc, "pos_in_stream") else 0

    line_no = exc.line if hasattr(exc, "line") else 1
    col = exc.column if hasattr(exc, "column") else None

    detail = "Invalid character"
    if hasattr(exc, "char"):
        detail = f"Unexpected character {exc.char!r}"
    elif hasattr(exc, "token"):
        detail = f"Unexpected token {str(exc.token)!r}"

    if col is None:
        return f"Line {line_no}: {detail} (near offset {pos})"

    # Build a one-line snippet around the offending position.
    lines = code.splitlines()
    src_line = lines[line_no - 1] if 0 <= line_no - 1 < len(lines) else ""
    caret = " " * max(col - 1, 0) + "^"

    return f"{detail} at line {line_no}, column {col}\n  {src_line}\n  {caret}"

def parse(code: str):
    with open(get_grammar_path(), "r") as f:
        grammar = f.read()

    parser = Lark(grammar, parser="earley")
    tree = parser.parse(code)
    
    transformer = AevixTransformer()
    result = transformer.transform(tree)
    
    return result