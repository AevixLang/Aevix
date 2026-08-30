from lark import Transformer, Token
from .ast import (
    Program, Let, Hot, Print,
    Number, Variable, Add, Sub, Mul, Div, Neg
)

class VeloTransformer(Transformer):
    # Atoms (NUMBER / CNAME / unary minus)
    def factor(self, items):
        if len(items) == 2:
            return Neg(value=items[1])
        tok = items[0]
        if tok.type == "NUMBER":
            return Number(value=int(tok))
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

    # Statements
    def stmt(self, items):
        return items[0]

    def expr_stmt(self, items):
        return items[0]

    def let_stmt(self, items):
        name = items[0]
        if isinstance(name, Variable):
            name = name.value
        return Let(name=str(name), value=items[1])

    def hot_block(self, items):
        return Hot(body=items)

    def print_stmt(self, items):
        return Print(value=items[0])

    # Program
    def start(self, items):
        return Program(body=items)
