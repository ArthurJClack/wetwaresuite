#include "achclwss/core/cli.hpp"

#include <cctype>
#include <istream>
#include <ostream>

namespace achclwss::cli {

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool inQuotes = false;
    bool any = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            current += line[++i];
            any = true;
        } else if (c == '"') {
            inQuotes = !inQuotes;
            any = true;
        } else if (!inQuotes && std::isspace(static_cast<unsigned char>(c))) {
            if (any) tokens.push_back(current);
            current.clear();
            any = false;
        } else {
            current += c;
            any = true;
        }
    }
    if (inQuotes) throw ParamError("unterminated quote");
    if (any) tokens.push_back(current);
    return tokens;
}

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

Value parseValue(const ParamSpec& spec, const std::string& text) {
    const std::string where = "parameter '" + spec.name + "'";
    try {
        switch (spec.type) {
            case ParamType::String: return text;
            case ParamType::Bool: {
                auto t = lower(text);
                if (t == "true" || t == "1" || t == "yes" || t == "on") return true;
                if (t == "false" || t == "0" || t == "no" || t == "off") return false;
                throw ParamError(where + " must be true or false");
            }
            case ParamType::Int: {
                std::size_t used = 0;
                auto v = std::stoll(text, &used, 0);
                if (used != text.size()) throw ParamError(where + " must be an integer");
                return static_cast<std::int64_t>(v);
            }
            case ParamType::Number: {
                std::size_t used = 0;
                double v = std::stod(text, &used);
                if (used != text.size()) throw ParamError(where + " must be a number");
                return v;
            }
            case ParamType::NumberList: {
                NumberList out;
                std::string item;
                auto flush = [&] {
                    if (item.empty()) return;
                    std::size_t used = 0;
                    out.push_back(std::stod(item, &used));
                    if (used != item.size()) throw ParamError(where + " must be numbers separated by commas");
                    item.clear();
                };
                for (char c : text) {
                    if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c))) flush();
                    else item += c;
                }
                flush();
                return out;
            }
        }
    } catch (const std::invalid_argument&) {
        throw ParamError(where + " must be " + toString(spec.type));
    } catch (const std::out_of_range&) {
        throw ParamError(where + " is out of range");
    }
    return text;
}

}  // namespace

Params parseArgs(const CommandInfo& info, const std::vector<std::string>& tokens) {
    Params params;
    for (const auto& token : tokens) {
        auto eq = token.find('=');
        if (eq == std::string::npos || eq == 0) {
            throw ParamError("expected key=value, got '" + token + "' (try: " + usage(info) + ")");
        }
        std::string key = token.substr(0, eq);
        const ParamSpec* spec = nullptr;
        for (const auto& p : info.params) {
            if (p.name == key) spec = &p;
        }
        if (!spec) throw ParamError("unknown parameter '" + key + "' for " + info.id + " (try: " + usage(info) + ")");
        params[key] = parseValue(*spec, token.substr(eq + 1));
    }
    return params;
}

std::string format(const Result& result) {
    std::string out = result.message;
    std::string value = toText(result.value);
    if (!value.empty() && value != result.message) out += (out.empty() ? "" : "\n") + value;
    for (const auto& [key, v] : result.data) out += (out.empty() ? "" : "\n") + key + ": " + toText(v);
    for (const auto& a : result.attachments) {
        out += (out.empty() ? "" : "\n") + std::string("attachment: ") + (a.name.empty() ? a.mime : a.name) + " (" +
               std::to_string(a.bytes.size()) + " bytes)";
    }
    return out;
}

std::string usage(const CommandInfo& info) {
    std::string out = info.id;
    for (const auto& p : info.params) {
        std::string hint = p.choices.empty() ? std::string("<") + toString(p.type) + ">" : "";
        if (!p.choices.empty()) {
            for (const auto& c : p.choices) hint += (hint.empty() ? "" : "|") + c;
        }
        std::string part = p.name + "=" + hint;
        out += " " + (p.required ? part : "[" + part + "]");
    }
    return out;
}

Result Batch::runLine(const std::string& rawLine) {
    std::string line = rawLine;
    if (auto hash = line.find('#'); hash != std::string::npos && (hash == 0 || std::isspace(static_cast<unsigned char>(line[hash - 1])))) {
        line = line.substr(0, hash);
    }
    std::vector<std::string> tokens = split(line);
    if (tokens.empty()) return Result::success("");

    std::string capture;
    if (tokens.size() >= 2 && tokens[tokens.size() - 2] == "->") {
        capture = tokens.back();
        tokens.resize(tokens.size() - 2);
    }
    for (auto& t : tokens) {
        // reverse order so $img2 is replaced before $img
        for (auto it = vars_.rbegin(); it != vars_.rend(); ++it) {
            const auto& [name, value] = *it;
            const std::string key = "$" + name;
            for (auto pos = t.find(key); pos != std::string::npos; pos = t.find(key, pos + value.size())) {
                t.replace(pos, key.size(), value);
            }
        }
    }

    const std::string command = tokens.front();
    std::vector<std::string> args(tokens.begin() + 1, tokens.end());
    Result result;
    if (command == "undo") {
        result = session_.history().undo() ? Result::success("undone") : Result::failure("nothing to undo");
    } else if (command == "redo") {
        result = session_.history().redo() ? Result::success("redone") : Result::failure("nothing to redo");
    } else if (command == "echo") {
        std::string text;
        for (const auto& a : args) text += (text.empty() ? "" : " ") + a;
        result = Result::success(text);
    } else {
        const CommandInfo* info = session_.registry().find(command);
        if (!info) return Result::failure("unknown command: " + command);
        try {
            result = session_.run(command, parseArgs(*info, args));
        } catch (const ParamError& e) {
            return Result::failure(e.what());
        }
    }
    if (result.ok && !capture.empty()) {
        const Value* doc = result.field("doc");
        vars_[capture] = doc ? toText(*doc) : toText(result.value);
    }
    return result;
}

int Batch::run(std::istream& in, std::ostream& out, std::ostream& err, bool keepGoing) {
    std::string line;
    int lineNo = 0;
    int failures = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Result r;
        try {
            r = runLine(line);
        } catch (const std::exception& e) {
            r = Result::failure(e.what());
        }
        if (r.ok) {
            auto text = format(r);
            if (!text.empty()) out << text << "\n";
        } else {
            ++failures;
            err << "line " << lineNo << ": " << r.message << "\n";
            if (!keepGoing) break;
        }
    }
    return failures;
}

}  // namespace achclwss::cli
