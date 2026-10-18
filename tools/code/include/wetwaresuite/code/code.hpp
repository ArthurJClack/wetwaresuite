// code: a light, many-language code editor you can drive headless. Files open as
// documents (code1) with undoable edits; a project folder gives a file tree,
// search and replace across files, and build/test/run with the compilers and
// interpreters already installed (found on PATH, never downloaded). A small
// table-driven highlighter and per-language outlines cover C/C++, Rust, Go,
// Python, JS/TS, Java, C#, Kotlin, Swift, Lua, shells, HTML/CSS, JSON/YAML/TOML,
// Markdown, SQL and more; compiler messages become clickable file:line errors;
// git status, diff, stage and commit run through git when it is installed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::code {

namespace fs = std::filesystem;

// ---- languages ------------------------------------------------------------------------

enum class Mode { Code, Markup, Markdown, Config, Plain };

struct Language {
    std::string id;    // "cpp"
    std::string name;  // "C++"
    std::vector<std::string> extensions;  // ".cpp", ".hpp"
    std::vector<std::string> filenames;   // "Makefile", "CMakeLists.txt"
    std::vector<std::string> lineComments;
    std::vector<std::pair<std::string, std::string>> blockComments;
    std::string quotes = "\"'";
    bool tripleQuotes = false;   // Python """docstrings"""
    bool multilineStrings = false;  // every quote may span lines (Go raw `...`, JS templates use `)
    bool caseInsensitive = false;   // SQL keywords
    bool preprocessor = false;      // # lines in C, C++, C#
    bool capitalTypes = false;      // Capitalised names are types (Rust, Go, Java, ...)
    bool colonKeys = false;         // name: value is a key (CSS properties, YAML)
    std::string variablePrefix;     // "$" in shells, PHP, Perl
    std::set<std::string> keywords, types, constants, builtins;
    Mode mode = Mode::Code;
};

const std::vector<Language>& languages();
const Language* languageById(const std::string& id);
// by file name and extension, then by a #! line in `head`; plain text when unknown
const Language& detectLanguage(const std::string& fileName, const std::string& head = {});
const Language& plainText();

// ---- highlighting ---------------------------------------------------------------------

enum class Token : int { Plain, Keyword, Type, Constant, Number, String, Comment, Preproc, Function, Tag, Attribute, Heading, Variable, Link };
const char* tokenName(Token t);

struct Span {
    int line = 0;  // 0-based
    int col = 0;   // byte offset in the line
    int len = 0;
    Token kind = Token::Plain;
};

// Spans for everything that isn't plain text, in order.
std::vector<Span> highlight(const std::string& text, const Language& lang);
std::string highlightHtml(const std::string& text, const Language& lang);
std::string highlightAnsi(const std::string& text, const Language& lang);

// ---- outline --------------------------------------------------------------------------

struct Symbol {
    int line = 0;  // 1-based
    std::string kind;  // function, class, struct, heading, ...
    std::string name;
    int depth = 0;
};
std::vector<Symbol> outline(const std::string& text, const Language& lang);

// ---- compiler and runtime messages ------------------------------------------------------

struct Problem {
    std::string file;  // as written in the message (made absolute with `cwd` when relative)
    int line = 0, col = 0;
    std::string severity;  // error, warning, note
    std::string message;
};
// gcc/clang, rustc, MSVC, tsc, go, javac, Python tracebacks, Node stack traces, ...
std::vector<Problem> parseProblems(const std::string& output, const fs::path& cwd = {});

// ---- toolchains --------------------------------------------------------------------------

struct Toolchain {
    std::string name;      // "g++"
    std::string program;   // full path
    std::string languages; // "C++"
};
std::vector<Toolchain> findToolchains();

struct Step {
    std::vector<std::string> args;
    fs::path cwd;
    std::string input;  // sent to the program's stdin, then closed (sqlite3 < file.sql)
    bool hasInput = false;
};
struct Plan {
    std::vector<Step> steps;
    std::string description;  // "compile with g++, then run"
};
// How to run one file; throws std::runtime_error saying what to install when nothing fits.
Plan planRun(const fs::path& file, const Language& lang, const std::vector<std::string>& args);
// How to build/test/run/clean a project folder (CMake, Cargo, Go, npm, Make, dotnet, Maven, Gradle, Meson).
Plan planProject(const fs::path& folder, const std::string& action);
// The project folder holding `file` (the nearest folder with a build file or .git), or "".
fs::path findProjectRoot(const fs::path& file);
std::string projectKind(const fs::path& folder);  // "cmake", "cargo", ... or ""

// Splits "a b \"c d\"" like a shell would (no expansion).
std::vector<std::string> splitArgs(const std::string& text);

void registerCommands(Session& session);

}  // namespace wetwaresuite::code
