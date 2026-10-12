// ============================================================================
// Aevix Diagnostic — unified error/warning/note reporting
//
// All compiler stages (sema, codegen, runtime) emit diagnostics through this
// interface. Format: level: message\n  --> file:line:col
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <iostream>

struct Diagnostic {
    enum Level { Error, Warning, Note };

    Level level;
    std::string message;
    std::string file;
    int line = 0;
    int col = 0;

    Diagnostic(Level lvl, std::string msg, std::string f = "", int l = 0, int c = 0)
        : level(lvl), message(std::move(msg)), file(std::move(f)), line(l), col(c) {}

    std::string format() const {
        std::ostringstream os;
        switch (level) {
            case Error:   os << "error"; break;
            case Warning: os << "warning"; break;
            case Note:    os << "note"; break;
        }
        os << ": " << message;
        if (!file.empty() && line > 0) {
            os << "\n  --> " << file << ":" << line << ":" << col;
        }
        return os.str();
    }
};

// Collector for diagnostics emitted during compilation.
class DiagnosticCollector {
public:
    void error(const std::string& msg, const std::string& file, int line, int col) {
        diags_.emplace_back(Diagnostic::Error, msg, file, line, col);
    }

    void warning(const std::string& msg, const std::string& file, int line, int col) {
        diags_.emplace_back(Diagnostic::Warning, msg, file, line, col);
    }

    void note(const std::string& msg, const std::string& file = "", int line = 0, int col = 0) {
        diags_.emplace_back(Diagnostic::Note, msg, file, line, col);
    }

    bool has_errors() const {
        for (const auto& d : diags_)
            if (d.level == Diagnostic::Error) return true;
        return false;
    }

    void emit(std::ostream& os = std::cerr) const {
        for (const auto& d : diags_)
            os << d.format() << "\n";
    }

    void clear() { diags_.clear(); }

    const std::vector<Diagnostic>& all() const { return diags_; }

private:
    std::vector<Diagnostic> diags_;
};
