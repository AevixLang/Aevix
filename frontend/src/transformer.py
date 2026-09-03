# ============================================================================
# Aevix Frontend: Lark Parse-Tree to AST Transformer
#
# This module implements the AevixTransformer, which walks the Lark parse tree
# and converts it into the structured AST defined in ast.py.
# ============================================================================
from lark import Transformer, Token
from .ast import (
    Program, Let, Hot, Print, If, While, For, ForIn, Return, Assign, FuncDecl, Param, Call,
    Number, Variable, Float, Bool, String, ArrayLit, Index, Add, Sub, Mul, Div, Neg,
    CmpOp, And, Or, Not
)

def _expr_from_token(tok):
    """Converts a raw Lark token into the corresponding AST Expression node."""
    if tok.type == "NUMBER":
        return Number(value=int(tok))
    elif tok.type == "FLOAT":
        return Float(value=float(tok))
    elif tok.type == "BOOL":
        return Bool(value=(str(tok) == "true"))
    elif tok.type == "STRING":
        return String(value=str(tok)[1:-1])
    return Variable(value=str(tok))

class AevixTransformer(Transformer):
    """
    Transformer that maps Lark grammar rules to Aevix AST nodes.
    Each method corresponds to a rule name in aevix.lark.
    """

    # ---- Atoms & Primaries ----
    def atom(self, items):
        if len(items) == 1 and isinstance(items[0], Token):
            return _expr_from_token(items[0])
        return items[0]  # "(" expr ")" grouping or array_lit

    def primary(self, items):
        """Handles variable access, function calls, and array indexing."""
        result = Variable(value=str(items[0]))
        for suffix in items[1:]:
            if isinstance(suffix, list):  # call argument list
                result = Call(callee=result.value, args=suffix)
            else:  # index suffix -> raw index expression
                result = Index(object=result, index=suffix)
        return result

    def suffix(self, items):
        return items[0]

    def call_suffix(self, items):
        return items[0] if items else []

    def index_suffix(self, items):
        return items[0]

    def array_lit(self, items):
        elements = items[0] if items else []
        return ArrayLit(elements=elements)

    def array_items(self, items):
        return list(items)

    def arg_list(self, items):
        return list(items)

    # ---- Unary Operators ----
    def factor(self, items):
        if len(items) == 2:
            op, operand = str(items[0]), items[1]
            return Neg(value=operand) if op == "-" else Not(value=operand)
        return items[0]

    # ---- Binary Operators ----
    def term(self, items):
        result = items[0]
        for i in range(1, len(items), 2):
            op, right = str(items[i]), items[i + 1]
            result = Mul(left=result, right=right) if op == '*' else Div(left=result, right=right)
        return result

    def arith(self, items):
        result = items[0]
        for i in range(1, len(items), 2):
            op, right = str(items[i]), items[i + 1]
            result = Add(left=result, right=right) if op == '+' else Sub(left=result, right=right)
        return result

    def comparision(self, items):
        result = items[0]
        for i in range(1, len(items), 2):
            op = str(items[i])
            result = CmpOp(op=op, left=result, right=items[i + 1])
        return result

    def and_(self, items):
        result = items[0]
        for i in range(1, len(items), 2):
            result = And(left=result, right=items[i + 1])
        return result

    def or_(self, items):
        result = items[0]
        for i in range(1, len(items), 2):
            result = Or(left=result, right=items[i + 1])
        return result

    def expr(self, items):
        return items[0]

    # ---- Types ----
    def type_name(self, items):
        return items[0]

    def scalar_type(self, items):
        return str(items[0])

    def array_type(self, items):
        base = items[0]
        size = str(items[1]) if len(items) > 1 else ""
        return f"{base}[{size}]"

    # ---- Statements ----
    def stmt(self, items):
        return items[0]

    def expr_stmt(self, items):
        return items[0]

    def let_stmt(self, items):
        name = str(items[0])
        var_type, value = None, items[1]
        if isinstance(value, str):
            var_type, value = value, items[2]
        return Let(name=name, value=value, var_type=var_type)

    def block(self, items):
        return list(items)

    def hot_block(self, items):
        return Hot(body=items[0])

    def print_stmt(self, items):
        return Print(value=items[0])

    def if_stmt(self, items):
        condition, then_body = items[0], items[1]
        else_body = items[2] if len(items) > 2 else None
        return If(condition=condition, then_body=then_body, else_body=else_body)

    def else_branch(self, items):
        branch = items[0]
        return [branch] if isinstance(branch, If) else list(branch)

    def while_stmt(self, items):
        return While(condition=items[0], body=items[1])

    def for_stmt(self, items):
        parts = list(items)
        init = parts[0] if len(parts) >= 1 else None
        cond = parts[1] if len(parts) >= 2 else None
        step = parts[2] if len(parts) >= 3 else None
        body = parts[-1]
        return For(init=init, condition=cond, step=step, body=body)

    def for_in_stmt(self, items):
        return ForIn(var=str(items[0]), iterable=items[1], body=items[2])

    def for_init(self, items):
        return items[0]

    def for_step(self, items):
        return items[0]

    def for_let(self, items):
        name = str(items[0])
        var_type, value = None, items[1]
        if isinstance(value, str):
            var_type, value = value, items[2]
        return Let(name=name, value=value, var_type=var_type)

    def for_assign(self, items):
        return Assign(name=items[0], value=items[1])

    def return_stmt(self, items):
        return Return(value=items[0]) if items else Return()

    def assign_stmt(self, items):
        return Assign(name=items[0], value=items[1])

    # ---- Functions ----
    def func_decl(self, items):
        name, params = str(items[0]), items[1]
        return_type, body = None, []
        idx = 2
        if idx < len(items) and isinstance(items[idx], str):
            return_type = items[idx]
            idx += 1
        body = items[idx]
        return FuncDecl(name=name, params=params, return_type=return_type, body=body)

    def params(self, items):
        return list(items)

    def param(self, items):
        name = str(items[0])
        var_type = items[1] if len(items) > 1 else None
        return Param(name=name, var_type=var_type)

    # ---- Program ----
    def start(self, items):
        return Program(body=items)
