from lark import Transformer, Token
from .ast import (
    Program, Let, Hot, Print,
    Number, Variable, Add, Sub, Mul, Div, Neg
)

class VeloTransformer(Transformer):
    # Terminals
    def term(self, items):
        tok = items[0]
        if tok.type == "NUMBER":
            return Number(value=int(tok))
        return Variable(value=str(tok))

    # Expressions
    def expr(self, items):
        if len(items) == 1:
            return items[0]

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
