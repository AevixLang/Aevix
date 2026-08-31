from lark import Transformer, Token
from .ast import (
    Program, Let, Hot, Print,
    Number, Variable, Float, Bool, String, Add, Sub, Mul, Div, Neg
)

class VeloTransformer(Transformer):
    # Atoms (NUMBER / FLOAT / BOOL / STRING / unary minus)
    def factor(self, items):
        if len(items) == 2:
            return Neg(value=items[1])
        tok = items[0]
        if tok.type == "NUMBER":
            return Number(value=int(tok))
        elif tok.type == "FLOAT":
            return Float(value=float(tok))
        elif tok.type == "BOOL":
            return Bool(value=(str(tok) == "true"))
        elif tok.type == "STRING":
            return String(value=str(tok)[1:-1])
        if str(tok) == "true":
            return Bool(value=True)
        if str(tok) == "false":
            return Bool(value=False)
        return Variable(value=str(tok))

    # mul/div
    def term(self, items):
        result = items[0]
        for i in range(1, len(items), 2):
            op = items[i]
            right = items[i + 1]
            if str(op) == '*':
                result = Mul(left=result, right=right)
            elif str(op) == '/':
                result = Div(left=result, right=right)
        return result

    # add/sub
    def expr(self, items):
        result = items[0]
        for i in range(1, len(items), 2):
            op = items[i]
            right = items[i + 1]
            if str(op) == '+':
                result = Add(left=result, right=right)
            elif str(op) == '-':
                result = Sub(left=result, right=right)
        return result

    # Types (type_name: "int" | "float" | "bool" | "string")
    def type_name(self, items):
        return str(items[0])

    # Statements
    def stmt(self, items):
        return items[0]

    def expr_stmt(self, items):
        return items[0]

    def let_stmt(self, items):
        name = items[0]
        if isinstance(name, Variable):
            name = name.value
        name = str(name)

        var_type = None
        value = items[1]
        if isinstance(value, str):
            var_type = value
            value = items[2]
        return Let(name=name, value=value, var_type=var_type)

    def hot_block(self, items):
        return Hot(body=items)

    def print_stmt(self, items):
        return Print(value=items[0])

    # Program
    def start(self, items):
        return Program(body=items)
