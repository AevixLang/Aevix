from dataclasses import dataclass, field
from typing import List, Union, Optional


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


# Expressions
@dataclass
class Number:
    value: int
    type: str = "Number"


@dataclass
class Variable:
    value: str
    type: str = "Variable"


@dataclass
class Add:
    left: object
    right: object
    type: str = "Add"


@dataclass
class Sub:
    left: object
    right: object
    type: str = "Sub"


@dataclass
class Mul:
    left: object
    right: object
    type: str = "Mul"


@dataclass
class Div:
    left: object
    right: object
    type: str = "Div"


@dataclass
class Neg:
    value: object
    type: str = "Neg"


# Statements
@dataclass
class Let:
    name: str
    value: object
    type: str = "Let"


@dataclass
class Hot:
    body: list
    type: str = "Hot"


@dataclass
class Print:
    value: object
    type: str = "Print"


@dataclass
class Program:
    body: list
    type: str = "Program"
