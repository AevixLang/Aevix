# ============================================================================
# Aevix Frontend: AST Node Definitions
#
# Defines the structure of the Abstract Syntax Tree (AST). Each language
# construct is represented by a dataclass. The `to_dict` utility converts
# the AST into a JSON schema consumed by the backend's json_reader.
# ============================================================================
from dataclasses import dataclass
from typing import List, Union, Optional

def to_dict(obj):
    """
    Recursively converts AST dataclasses and lists into dictionaries
    for JSON serialization.
    """
    if hasattr(obj, "__dataclass_fields__"):
        d = {"type": obj.type}
        for k, v in obj.__dict__.items():
            if k != "type":
                d[k] = to_dict(v)
        return d
    elif isinstance(obj, list):
        return [to_dict(item) for item in obj]
    return obj

# ----------------------------------------------------------------------------
# Expressions
# ----------------------------------------------------------------------------

@dataclass
class Number:
    value: int
    type: str = "Number"

@dataclass
class Variable:
    value: str
    type: str = "Variable"

@dataclass
class Float:
    value: float
    type: str = "Float"

@dataclass
class Bool:
    value: bool
    type: str = "Bool"

@dataclass
class String:
    value: str
    type: str = "String"

@dataclass
class ArrayLit:
    elements: list
    type: str = "ArrayLit"

@dataclass
class Index:
    object: object
    index: object
    type: str = "Index"

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

@dataclass
class CmpOp:
    op: str
    left: object
    right: object
    type: str = "CmpOp"

@dataclass
class And:
    left: object
    right: object
    type: str = "And"

@dataclass
class Or:
    left: object
    right: object
    type: str = "Or"

@dataclass
class Not:
    value: object
    type: str = "Not"

@dataclass
class Call:
    callee: str
    args: list
    type: str = "Call"

@dataclass
class MemberAccess:
    object: object
    member: str
    type: str = "MemberAccess"

@dataclass
class StructLiteral:
    name: str
    args: list
    type: str = "StructLiteral"

@dataclass
class New:
    base: str
    size: object
    type: str = "New"

# ----------------------------------------------------------------------------
# Statements
# ----------------------------------------------------------------------------

@dataclass
class Let:
    name: str
    value: object
    var_type: Optional[str] = None
    type: str = "Let"

@dataclass
class Hot:
    body: list
    type: str = "Hot"

@dataclass
class Epoch:
    body: list
    type: str = "Epoch"

@dataclass
class Print:
    value: object
    type: str = "Print"

@dataclass
class If:
    condition: object
    then_body: list
    else_body: Optional[list] = None
    type: str = "If"

@dataclass
class While:
    condition: object
    body: list
    type: str = "While"

@dataclass
class For:
    body: list
    init: Optional[object] = None
    condition: Optional[object] = None
    step: Optional[object] = None
    type: str = "For"

@dataclass
class ForIn:
    var: str
    iterable: object
    body: list
    type: str = "ForIn"

@dataclass
class Return:
    value: Optional[object] = None
    type: str = "Return"

@dataclass
class Assign:
    name: object
    value: object
    type: str = "Assign"

@dataclass
class Param:
    name: str
    var_type: Optional[str] = None
    type: str = "Param"

@dataclass
class FuncDecl:
    name: str
    params: list
    body: list
    return_type: Optional[str] = None
    type: str = "FuncDecl"

@dataclass
class StructField:
    name: str
    var_type: Optional[str] = None
    type: str = "StructField"

@dataclass
class StructDecl:
    name: str
    fields: list
    type: str = "StructDecl"

@dataclass
class Import:
    spec: str
    type: str = "Import"

@dataclass
class Program:
    body: list
    type: str = "Program"
