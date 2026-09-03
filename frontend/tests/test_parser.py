import json
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from src.parser import parse, format_parse_error


def to_dict(obj):
    if hasattr(obj, "__dataclass_fields__"):
        d = {"type": obj.type}
        for k, v in obj.__dict__.items():
            if k != "type":
                d[k] = to_dict(v)
        return d
    elif isinstance(obj, list):
        return [to_dict(item) for item in obj]
    else:
        return obj


def parse_dict(code):
    return to_dict(parse(code))


def test_simple_if_stmt():
    ast = parse_dict("let x = 10;\nif (x > 5) { print x; } else { print 0; }\n")
    body = ast["body"]
    assert body[0]["type"] == "Let"
    if_stmt = body[1]
    assert if_stmt["type"] == "If"
    assert if_stmt["condition"]["type"] == "CmpOp"
    assert if_stmt["condition"]["op"] == ">"
    assert if_stmt["then_body"][0]["type"] == "Print"
    assert if_stmt["else_body"][0]["type"] == "Print"


def test_if_no_else():
    ast = parse_dict("if (x != 5) { print 1; }\n")
    if_stmt = ast["body"][0]
    assert if_stmt["type"] == "If"
    assert if_stmt["condition"]["op"] == "!="
    assert if_stmt["else_body"] is None


def test_if_chained_comparisons():
    ast = parse_dict("if (a >= b) { print a; }\n")
    cond = ast["body"][0]["condition"]
    assert cond["op"] == ">="
    assert cond["left"]["type"] == "Variable"
    assert cond["right"]["type"] == "Variable"


def test_nested_if():
    ast = parse_dict(
        "if (a < 10) { if (a > 2) { print 1; } else { print 2; } } else { print 3; }\n"
    )
    outer = ast["body"][0]
    assert outer["type"] == "If"
    inner = outer["then_body"][0]
    assert inner["type"] == "If"
    assert inner["else_body"][0]["type"] == "Print"


def test_else_if_chain():
    ast = parse_dict(
        "if (a >= 90) { print 1; } else if (a >= 80) { print 2; } else { print 3; }\n"
    )
    outer = ast["body"][0]
    assert outer["type"] == "If"
    inner = outer["else_body"][0]
    assert inner["type"] == "If"
    assert inner["condition"]["op"] == ">="
    assert inner["else_body"][0]["type"] == "Print"


def test_multiple_else_if():
    ast = parse_dict(
        "if (a >= 90) { print 1; } else if (a >= 80) { print 2; } "
        "else if (a >= 70) { print 3; } else { print 4; }\n"
    )
    lvl1 = ast["body"][0]
    lvl2 = lvl1["else_body"][0]
    lvl3 = lvl2["else_body"][0]
    assert lvl1["type"] == "If"
    assert lvl2["type"] == "If"
    assert lvl3["type"] == "If"
    assert lvl3["else_body"][0]["type"] == "Print"


def test_func_decl_typed():
    ast = parse_dict("func add(a: int, b: int) : int { return a + b; }\n")
    fd = ast["body"][0]
    assert fd["type"] == "FuncDecl"
    assert fd["name"] == "add"
    assert fd["return_type"] == "int"
    assert len(fd["params"]) == 2
    assert fd["params"][0]["name"] == "a"
    assert fd["params"][0]["var_type"] == "int"
    assert fd["body"][0]["type"] == "Return"
    assert fd["body"][0]["value"]["type"] == "Add"


def test_func_decl_void():
    ast = parse_dict("func greet(name: string) { print name; }\n")
    fd = ast["body"][0]
    assert fd["type"] == "FuncDecl"
    assert fd["return_type"] is None
    assert fd["params"][0]["var_type"] == "string"


def test_typed_fixed_array_let():
    ast = parse_dict("let x: int[3] = [1, 2, 3];\n")
    let = ast["body"][0]
    assert let["type"] == "Let"
    assert let["var_type"] == "int[3]"


def test_typed_open_array_let():
    ast = parse_dict("let y: float[] = [1.5, 2.5];\n")
    let = ast["body"][0]
    assert let["type"] == "Let"
    assert let["var_type"] == "float[]"


def test_typed_array_func_signature():
    ast = parse_dict("func f(a: int[2]) : int[3] { return [0, 0, 0]; }\n")
    fd = ast["body"][0]
    assert fd["type"] == "FuncDecl"
    assert fd["params"][0]["var_type"] == "int[2]"
    assert fd["return_type"] == "int[3]"


def test_func_call():
    ast = parse_dict("print add(4, 5);\n")
    call = ast["body"][0]["value"]
    assert call["type"] == "Call"
    assert call["callee"] == "add"
    assert len(call["args"]) == 2
    assert call["args"][0]["value"] == 4


def test_while_stmt():
    ast = parse_dict("while (i < 10) { i = i + 1; }\n")
    w = ast["body"][0]
    assert w["type"] == "While"
    assert w["condition"]["op"] == "<"
    assign = w["body"][0]
    assert assign["type"] == "Assign"
    assert assign["name"] == {"type": "Variable", "value": "i"}


def test_for_stmt():
    ast = parse_dict(
        "for (let i = 0; i < 10; i = i + 1) { print i; }\n"
    )
    f = ast["body"][0]
    assert f["type"] == "For"
    assert f["init"]["type"] == "Let"
    assert f["condition"]["op"] == "<"
    assert f["step"]["type"] == "Assign"
    assert f["step"]["value"]["type"] == "Add"
    assert f["body"][0]["type"] == "Print"


def test_for_assign_init():
    ast = parse_dict(
        "let i = 0;\nfor (i = 0; i < 10; i = i + 1) { print i; }\n"
    )
    f = ast["body"][1]
    assert f["type"] == "For"
    assert f["init"]["type"] == "Assign"


def test_return_stmt_bare():
    ast = parse_dict("func f() { return; }\n")
    fd = ast["body"][0]
    assert fd["body"][0]["type"] == "Return"
    assert fd["body"][0]["value"] is None


def test_logical_and():
    ast = parse_dict("if (a > 1 && b < 5) { print 1; }\n")
    cond = ast["body"][0]["condition"]
    assert cond["type"] == "And"
    assert cond["left"]["type"] == "CmpOp"
    assert cond["right"]["type"] == "CmpOp"


def test_logical_or():
    ast = parse_dict("if (a == 1 || b == 2) { print 1; }\n")
    cond = ast["body"][0]["condition"]
    assert cond["type"] == "Or"
    assert cond["left"]["type"] == "CmpOp"


def test_logical_not():
    ast = parse_dict("if (!flag) { print 1; }\n")
    cond = ast["body"][0]["condition"]
    assert cond["type"] == "Not"
    assert cond["value"]["type"] == "Variable"


def test_neg_vs_not():
    ast = parse_dict("let n = -x; let b = !y;\n")
    assert ast["body"][0]["value"]["type"] == "Neg"
    assert ast["body"][1]["value"]["type"] == "Not"


def test_bool_literal():
    ast = parse_dict("let f: bool = true; let g: bool = false;\n")
    assert ast["body"][0]["value"]["type"] == "Bool"
    assert ast["body"][0]["value"]["value"] is True
    assert ast["body"][1]["value"]["value"] is False


def _parse_error_text(code):
    try:
        parse(code)
    except Exception as e:
        return format_parse_error(code, e)
    return None


def test_format_parse_error_shows_position():
    code = "let x = 5;\nlet y = ;\n"
    msg = _parse_error_text(code)
    assert msg is not None
    assert "line 2" in msg
    assert "column" in msg
    assert "^" in msg


def test_format_parse_error_unexpected_char():
    code = "let z = @;\n"
    msg = _parse_error_text(code)
    assert msg is not None
    assert "character" in msg


def test_format_parse_error_non_lark():
    msg = format_parse_error("abc", RuntimeError("boom"))
    assert msg == "boom"


def test_line_and_block_comments_ignored():
    code = ("// line comment\n"
            "let a: int = 1; /* block */ let b: int = 2; // trailing\n"
            "/*\n  multi-line\n*/\n"
            "let c: int = a + b;\n")
    ast = parse_dict(code)
    assert len(ast["body"]) == 3
    assert ast["body"][2]["name"] == "c"


def test_array_literal_ast():
    ast = parse_dict("let a = [1, 2, 3];\n")
    value = ast["body"][0]["value"]
    assert value["type"] == "ArrayLit"
    assert [e["value"] for e in value["elements"]] == [1, 2, 3]


def test_array_index_read_ast():
    ast = parse_dict("print a[1];\n")
    value = ast["body"][0]["value"]
    assert value["type"] == "Index"
    assert value["object"] == {"type": "Variable", "value": "a"}
    assert value["index"] == {"type": "Number", "value": 1}


def test_array_nested_index_ast():
    ast = parse_dict("print m[1][0];\n")
    value = ast["body"][0]["value"]
    assert value["type"] == "Index"
    assert value["object"]["type"] == "Index"
    assert value["object"]["index"]["value"] == 1
    assert value["index"]["value"] == 0


def test_array_indexed_assign_ast():
    ast = parse_dict("a[0] = 10;\n")
    assign = ast["body"][0]
    assert assign["type"] == "Assign"
    assert assign["name"]["type"] == "Index"
    assert assign["name"]["index"] == {"type": "Number", "value": 0}
    assert assign["value"] == {"type": "Number", "value": 10}


def test_array_empty_literal_ast():
    ast = parse_dict("let a = [];\n")
    assert ast["body"][0]["value"] == {"type": "ArrayLit", "elements": []}
