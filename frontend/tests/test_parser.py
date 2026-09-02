import json
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from src.parser import parse


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
