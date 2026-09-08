// ============================================================================
// Aevix Backend: Compiler Entry Point
//
// This module is the main driver for the backend. It parses the AST from JSON,
// invokes the CodeGenerator to emit LLVM IR, and handles top-level errors.
// ============================================================================
#include "json_reader.hpp"
#include "codegen.hpp"
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: aevix-backend <ast.json>" << std::endl;
        return 1;
    }

    // Step 1: Parse AST from JSON
    Program program = parse_json(argv[1]);

    try {
        // Step 2: Generate LLVM IR
        CodeGenerator generator;
        generator.generate_program(program);
        generator.finalize();
    } catch (const std::runtime_error& e) {
        std::cerr << "❌ Codegen error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
