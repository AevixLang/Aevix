import os
from lark import Lark
from .transformer import AevixTransformer

def get_grammar_path():
    current_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(current_dir, "..", "grammar", "aevix.lark")

def parse(code: str):
    with open(get_grammar_path(), "r") as f:
        grammar = f.read()

    parser = Lark(grammar, parser="lalr")
    tree = parser.parse(code)
    
    transformer = AevixTransformer()
    result = transformer.transform(tree)
    
    return result