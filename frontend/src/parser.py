import os
from lark import Lark
from .transformer import VeloTransformer

def get_grammar_path():
    current_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(current_dir, "..", "grammar", "velo.lark")

def parse(code: str):
    with open(get_grammar_path(), "r") as f:
        grammar = f.read()

    parser = Lark(grammar, parser="lalr")
    tree = parser.parse(code)
    
    transformer = VeloTransformer()
    result = transformer.transform(tree)
    
    return result