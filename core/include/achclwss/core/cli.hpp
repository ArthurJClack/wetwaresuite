// Headless command line for any registered command:
//   photo.resize doc=img1 width=800 height=600
//   photo.open path="my photo.jpg" -> pic      (captures the new doc id as $pic)
// The same syntax works one-shot (`achclwss run ...`) and in batch scripts.
#pragma once

#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "achclwss/core/command.hpp"

namespace achclwss::cli {

// Splits a line into tokens, honouring "double quotes" and backslash escapes.
std::vector<std::string> split(const std::string& line);

// Turns key=value tokens into typed params using the command's spec. Throws ParamError.
Params parseArgs(const CommandInfo& info, const std::vector<std::string>& tokens);

// What the CLI prints for a result: the message, then key: value lines for data.
std::string format(const Result& result);

// One-line usage for a command, e.g. "photo.resize doc=<string> width=<integer> [filter=<...>]".
std::string usage(const CommandInfo& info);

// Runs batch scripts: one command per line, '#' comments, `-> name` captures,
// `$name` substitution, and the built-ins undo, redo and echo.
class Batch {
public:
    explicit Batch(Session& session) : session_(session) {}

    Result runLine(const std::string& line);
    // Runs every line; stops at the first failure unless keepGoing. Returns failures count.
    int run(std::istream& in, std::ostream& out, std::ostream& err, bool keepGoing = false);

    const std::map<std::string, std::string>& vars() const { return vars_; }

private:
    Session& session_;
    std::map<std::string, std::string> vars_;
};

}  // namespace achclwss::cli
