#include "ast.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "importer.hpp"
#include "json_out.hpp"
#include <iostream>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    std::string out_path = "ast.json";
    std::string src_path;

    // Parse args
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg.starts_with("-")) {
            std::cerr << "Unknown flag: " << arg << "\n";
            return 1;
        } else {
            src_path = arg;
        }
    }

    if (src_path.empty()) {
        std::cerr << "Usage: aevix-frontend <file.aev> [-o <output.json>]\n";
        return 1;
    }

    try {
        aevix::ImportResolver resolver;
        aevix::Program prog = resolver.load(src_path);
        std::string json = aevix::JsonWriter::to_json(prog);

        std::ofstream out(out_path);
        out << json;
        out.close();

        std::cout << "AST saved to " << out_path << "\n";
    } catch (const aevix::ImportError& e) {
        std::cerr << "Import error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Parse error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
