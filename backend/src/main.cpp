#include "json_reader.hpp"
#include "codegen.hpp"
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: aevix-backend <ast.json>" << std::endl;
        return 1;
    }

    Program program = parse_json(argv[1]);

    try {
        CodeGenerator generator;
        generator.generate_program(program);
        generator.finalize();
    } catch (const std::runtime_error& e) {
        std::cerr << "❌ Codegen error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
