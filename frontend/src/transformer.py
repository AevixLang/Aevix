# ============================================================================
# Aevix frontend — Lark parse-tree -> AST transformer
#
# Each method below mirrors one grammar rule and returns the matching AST node
# (see ast.py). Section dividers group atoms, unary/binary operators, types,
# statements, functions and program-level rules.
# ============================================================================
from lark import Transformer, Token
from .ast import (
    Program, Let, Hot, Print, If, While, For, Return, Assign, FuncDecl, Param, Call,
    Number, Variable, Float, Bool, String, Add, Sub, Mul, Div, Neg, CmpOp, And, Or, Not
)


def _expr_from_token(tok):
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
    # ---- Atoms ----
    def atom(self, items):
        if len(items) == 1 and isinstance(items[0], Token):
            return _expr_from_token(items[0])
        if len(items) == 1:
            return items[0]  # "(" expr ")" grouping
        # CNAME call_suffix -> items = [CNAME, call_result]
        if len(items) == 2:
            return Call(callee=str(items[0]), args=items[1])
        return None

    def call_suffix(self, items):
        if not items:
            return []
        return items[0]

    def arg_list(self, items):
        return list(items)

    # ---- Unary (-, !) ----
    def factor(self, items):
        if len(items) == 2:
            op = str(items[0])
            operand = items[1]
            if op == "-":
                return Neg(value=operand)
            return Not(value=operand)
        return items[0]

    # ---- Binary operators by precedence: -*/ +- cmp and or ----
    def term(self, items):
        result = items[0]
        for i in range(1, len(items), 2):
            op = str(items[i])
            right = items[i + 1]
            if op == '*':
                result = Mul(left=result, right=right)
            elif op == '/':
                result = Div(left=result, right=right)
        return result

    def arith(self, items):
        result = items[0]
        for i in range(1, len(items), 2):
            op = str(items[i])
            right = items[i + 1]
            if op == '+':
                result = Add(left=result, right=right)
            elif op == '-':
                result = Sub(left=result, right=right)
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
        return str(items[0])

    # ---- Statements ----
    def stmt(self, items):
        return items[0]

    def expr_stmt(self, items):
        return items[0]

    def let_stmt(self, items):
        name = str(items[0])
        var_type = None
        value = items[1]
        if isinstance(value, str):
            var_type = value
            value = items[2]
        return Let(name=name, value=value, var_type=var_type)

    def block(self, items):
        return list(items)

    def hot_block(self, items):
        return Hot(body=items[0])

    def print_stmt(self, items):
        return Print(value=items[0])

    def if_stmt(self, items):
        condition = items[0]
        then_body = items[1]
        else_body = None
        if len(items) > 2:
            else_body = items[2]
        return If(condition=condition, then_body=then_body, else_body=else_body)

    def else_branch(self, items):
        branch = items[0]
        if isinstance(branch, If):
            return [branch]
        return list(branch)

    def while_stmt(self, items):
        return While(condition=items[0], body=items[1])

    def for_stmt(self, items):
        parts = list(items)
        init, cond, step = None, None, None
        if len(parts) >= 1:
            init = parts[0]
        if len(parts) >= 2:
            cond = parts[1]
        if len(parts) >= 3:
            step = parts[2]
        body = parts[-1]
        return For(init=init, condition=cond, step=step, body=body)

    def for_init(self, items):
        return items[0]

    def for_step(self, items):
        return items[0]

    def for_let(self, items):
        name = str(items[0])
        var_type = None
        value = items[1]
        if isinstance(value, str):
            var_type = value
            value = items[2]
        return Let(name=name, value=value, var_type=var_type)

    def for_assign(self, items):
        return Assign(name=str(items[0]), value=items[1])

    def return_stmt(self, items):
        if items:
            return Return(value=items[0])
        return Return()

    def assign_stmt(self, items):
        return Assign(name=str(items[0]), value=items[1])

    # ---- Functions ----
    def func_decl(self, items):
        name = str(items[0])
        params = items[1]
        return_type = None
        body = []
        # items: [name, params, optional_type, block]
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
        var_type = None
        if len(items) > 1:
            var_type = items[1]
        return Param(name=name, var_type=var_type)

    # ---- Program ----
    def start(self, items):
        return Program(body=items)