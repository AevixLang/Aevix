#include "json_reader.hpp"
#include "codegen.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: aevix-backend <ast.json>" << std::endl;
        return 1;
    }

    Program program = parse_json(argv[1]);

    CodeGenerator generator;
    generator.generate_program(program);
    generator.finalize();

    return 0;
}
