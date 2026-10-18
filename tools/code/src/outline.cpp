// Symbol outlines (one regular expression per kind of definition and language)
// and compiler/runtime messages turned into file:line problems.
#include <map>
#include <regex>

#include "wetwaresuite/code/code.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

namespace wetwaresuite::code {

namespace {

struct Rule {
    std::regex re;
    std::string kind;
    int nameGroup = 1;
    int depthGroup = 0;  // a group whose length gives the depth (indent, ###)
};

Rule rule(const char* pattern, const char* kind, int nameGroup = 1, int depthGroup = 0, bool icase = false) {
    auto flags = std::regex::ECMAScript | std::regex::optimize;
    if (icase) flags |= std::regex::icase;
    return Rule{std::regex(pattern, flags), kind, nameGroup, depthGroup};
}

const std::vector<Rule>& rulesFor(const std::string& id) {
    static const std::map<std::string, std::vector<Rule>> table = [] {
        std::map<std::string, std::vector<Rule>> t;
        const char* cFunc = R"(^\s*(?:(?:static|inline|extern|virtual|constexpr|consteval|explicit|friend|const|unsigned|signed|struct|enum)\s+)*[A-Za-z_][\w:<>,\s\*&]*?[\s\*&]+\**&?([A-Za-z_~][\w:~]*)\s*\([^;]*$)";
        t["c"] = {rule(R"(^\s*(?:typedef\s+)?(?:struct|union|enum)\s+([A-Za-z_]\w*)\s*\{?\s*$)", "struct"),
                  rule(R"(^#\s*define\s+([A-Za-z_]\w*))", "macro"), rule(cFunc, "function")};
        t["cpp"] = {rule(R"(^\s*(?:template\s*<[^>]*>\s*)?(?:class|struct|union)\s+(?:\[\[[^\]]*\]\]\s*)?([A-Za-z_]\w*)[^;]*$)", "class"),
                    rule(R"(^\s*enum\s+(?:class\s+|struct\s+)?([A-Za-z_]\w*)[^;]*$)", "enum"),
                    rule(R"(^\s*namespace\s+([A-Za-z_][\w:]*))", "namespace"), rule(R"(^#\s*define\s+([A-Za-z_]\w*))", "macro"),
                    rule(cFunc, "function")};
        t["rust"] = {rule(R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(?:const\s+)?(?:async\s+)?(?:unsafe\s+)?(?:extern\s+"[^"]*"\s+)?fn\s+([A-Za-z_]\w*))", "function"),
                     rule(R"(^\s*(?:pub(?:\([^)]*\))?\s+)?struct\s+([A-Za-z_]\w*))", "struct"),
                     rule(R"(^\s*(?:pub(?:\([^)]*\))?\s+)?enum\s+([A-Za-z_]\w*))", "enum"),
                     rule(R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(?:unsafe\s+)?trait\s+([A-Za-z_]\w*))", "trait"),
                     rule(R"(^\s*impl(?:<[^>]*>)?\s+(.+?)\s*\{)", "impl"), rule(R"(^\s*(?:pub(?:\([^)]*\))?\s+)?mod\s+([A-Za-z_]\w*))", "module"),
                     rule(R"(^\s*macro_rules!\s*([A-Za-z_]\w*))", "macro")};
        t["go"] = {rule(R"(^func\s+(?:\([^)]*\)\s*)?([A-Za-z_]\w*))", "function"), rule(R"(^type\s+([A-Za-z_]\w*)\s+(?:struct|interface))", "type"),
                   rule(R"(^type\s+([A-Za-z_]\w*)\s)", "type")};
        t["python"] = {rule(R"(^(\s*)(?:async\s+)?def\s+([A-Za-z_]\w*))", "function", 2, 1), rule(R"(^(\s*)class\s+([A-Za-z_]\w*))", "class", 2, 1)};
        std::vector<Rule> js = {rule(R"(^\s*(?:export\s+)?(?:default\s+)?(?:async\s+)?function\s*\*?\s*([A-Za-z_$][\w$]*))", "function"),
                                rule(R"(^\s*(?:export\s+)?(?:default\s+)?(?:abstract\s+)?class\s+([A-Za-z_$][\w$]*))", "class"),
                                rule(R"(^\s*(?:export\s+)?(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=\s*(?:async\s+)?(?:\([^)]*\)|[A-Za-z_$][\w$]*)\s*=>)", "function"),
                                rule(R"(^\s+(?:static\s+)?(?:async\s+)?(?!if\b|for\b|while\b|switch\b|catch\b|return\b)([A-Za-z_$][\w$]*)\s*\([^)]*\)\s*\{)", "method")};
        t["javascript"] = js;
        t["typescript"] = js;
        t["typescript"].push_back(rule(R"(^\s*(?:export\s+)?interface\s+([A-Za-z_$][\w$]*))", "interface"));
        t["typescript"].push_back(rule(R"(^\s*(?:export\s+)?type\s+([A-Za-z_$][\w$]*)\s*=)", "type"));
        t["typescript"].push_back(rule(R"(^\s*(?:export\s+)?(?:const\s+)?enum\s+([A-Za-z_$][\w$]*))", "enum"));
        std::vector<Rule> jvm = {rule(R"(^\s*(?:(?:public|private|protected|internal|static|final|abstract|sealed|partial|data|open|inner|readonly)\s+)*(?:class|interface|enum|record|struct|object)\s+([A-Za-z_]\w*))", "class"),
                                 rule(R"(^\s*(?:(?:public|private|protected|internal|static|final|abstract|override|virtual|async|synchronized|native|sealed|extern|unsafe|new)\s+)+[\w<>\[\],\.\? ]+\s+([A-Za-z_]\w*)\s*\([^;]*$)", "method")};
        t["java"] = jvm;
        t["csharp"] = jvm;
        t["scala"] = {rule(R"(^\s*(?:case\s+)?(?:class|object|trait|enum)\s+([A-Za-z_]\w*))", "class"), rule(R"(^\s*(?:override\s+)?def\s+([A-Za-z_]\w*))", "function")};
        t["kotlin"] = {jvm[0], rule(R"(^\s*(?:(?:public|private|protected|internal|override|suspend|inline|open|operator|infix)\s+)*fun\s+(?:<[^>]*>\s*)?(?:[\w\.]+\.)?([A-Za-z_]\w*))", "function")};
        t["swift"] = {rule(R"(^\s*(?:(?:public|private|fileprivate|internal|open|final)\s+)*(?:class|struct|enum|protocol|extension|actor)\s+([A-Za-z_]\w*))", "class"),
                      rule(R"(^\s*(?:(?:public|private|fileprivate|internal|open|static|override|mutating|final)\s+)*func\s+([A-Za-z_]\w*))", "function")};
        t["dart"] = {rule(R"(^\s*(?:abstract\s+)?(?:class|mixin|enum|extension)\s+([A-Za-z_]\w*))", "class"),
                     rule(R"(^\s*(?:static\s+)?(?:Future<[^>]*>|[\w<>\?]+)\s+([A-Za-z_]\w*)\s*\([^;]*\)\s*(?:async\s*)?\{)", "function")};
        t["zig"] = {rule(R"(^\s*(?:pub\s+)?(?:export\s+)?(?:inline\s+)?fn\s+([A-Za-z_]\w*))", "function"), rule(R"(^\s*(?:pub\s+)?const\s+([A-Za-z_]\w*)\s*=\s*(?:struct|enum|union))", "struct")};
        t["lua"] = {rule(R"(^\s*(?:local\s+)?function\s+([\w\.:]+))", "function"), rule(R"(^\s*(?:local\s+)?([\w\.]+)\s*=\s*function)", "function")};
        t["ruby"] = {rule(R"(^(\s*)def\s+([\w\.\?!=]+))", "function", 2, 1), rule(R"(^(\s*)(?:class|module)\s+([\w:]+))", "class", 2, 1)};
        t["php"] = {rule(R"(^\s*(?:(?:public|private|protected|static|abstract|final)\s+)*function\s+&?([A-Za-z_]\w*))", "function"),
                    rule(R"(^\s*(?:abstract\s+|final\s+)?(?:class|interface|trait|enum)\s+([A-Za-z_]\w*))", "class")};
        t["perl"] = {rule(R"(^\s*sub\s+([\w:]+))", "function"), rule(R"(^\s*package\s+([\w:]+))", "package")};
        t["shell"] = {rule(R"(^\s*(?:function\s+)?([A-Za-z_][\w\-\.]*)\s*\(\)\s*\{?)", "function"), rule(R"(^\s*function\s+([A-Za-z_][\w\-\.]*))", "function")};
        t["fish"] = {rule(R"(^\s*function\s+([\w\-]+))", "function")};
        t["powershell"] = {rule(R"(^\s*function\s+([\w\-]+))", "function", 1, 0, true), rule(R"(^\s*class\s+(\w+))", "class", 1, 0, true)};
        t["batch"] = {rule(R"(^:([A-Za-z_]\w*))", "label")};
        t["r"] = {rule(R"(^\s*([\w\.]+)\s*(?:<-|=)\s*function)", "function")};
        t["julia"] = {rule(R"(^\s*function\s+([\w\.!]+))", "function"), rule(R"(^\s*(?:mutable\s+)?struct\s+(\w+))", "struct"), rule(R"(^\s*module\s+(\w+))", "module")};
        t["elixir"] = {rule(R"(^\s*defmodule\s+([\w\.]+))", "module"), rule(R"(^\s*defp?\s+([\w\?!]+))", "function")};
        t["haskell"] = {rule(R"(^([a-z_][\w']*)\s*::)", "function"), rule(R"(^(?:data|newtype|type|class)\s+([A-Z][\w']*))", "type")};
        t["markdown"] = {rule(R"(^(#{1,6})\s+(.+?)\s*#*\s*$)", "heading", 2, 1)};
        t["sql"] = {rule(R"(^\s*create\s+(?:or\s+replace\s+)?(?:temp(?:orary)?\s+)?(table|view|function|procedure|index|trigger|type|schema)\s+(?:if\s+not\s+exists\s+)?([\w\.\"`\[\]]+))", "table", 2, 0, true)};
        t["css"] = {rule(R"(^\s*(@media[^{]*|@keyframes\s+[\w\-]+|[^@\s{}/][^{};/]*?)\s*\{)", "rule")};
        t["html"] = {rule(R"(<h([1-6])[^>]*>([^<]+))", "heading", 2, 1, true), rule(R"(\bid\s*=\s*"([^"]+)\")", "id")};
        t["xml"] = {rule(R"(\bid\s*=\s*"([^"]+)\")", "id")};
        t["yaml"] = {rule(R"(^([A-Za-z_][\w\-\.]*)\s*:)", "key")};
        t["toml"] = {rule(R"(^\s*\[\[?([^\]]+)\]\]?)", "section")};
        t["ini"] = {rule(R"(^\s*\[([^\]]+)\])", "section")};
        t["makefile"] = {rule(R"(^([\w\.\-/%\$\(\)]+)\s*:(?!=))", "target")};
        t["cmake"] = {rule(R"(^\s*(?:function|macro)\s*\(\s*([\w\-]+))", "function", 1, 0, true),
                      rule(R"(^\s*add_(?:executable|library|custom_target)\s*\(\s*([\w\-\.]+))", "target", 1, 0, true)};
        t["dockerfile"] = {rule(R"(^\s*FROM\s+(\S+(?:\s+AS\s+\S+)?))", "stage", 1, 0, true)};
        return t;
    }();
    static const std::vector<Rule> none;
    auto it = table.find(id);
    return it == table.end() ? none : it->second;
}

bool controlWord(const std::string& name) {
    static const std::set<std::string> words = {"if", "for", "while", "switch", "return", "else", "catch", "sizeof", "do", "case", "new", "delete"};
    return words.count(name) > 0;
}

}  // namespace

std::vector<Symbol> outline(const std::string& text, const Language& lang) {
    const auto& rules = rulesFor(lang.id);
    std::vector<Symbol> out;
    if (rules.empty()) return out;
    auto lines = sysutil::splitLines(text);
    bool inBlockComment = false;
    bool fence = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.size() > 2000) continue;  // minified code: not worth it
        if (lang.id == "markdown" && (line.rfind("```", 0) == 0 || line.rfind("~~~", 0) == 0)) fence = !fence;
        if (fence) continue;
        // skip comment lines so commented-out code doesn't show
        std::string t = sysutil::trim(line);
        if (inBlockComment) {
            if (t.find("*/") != std::string::npos) inBlockComment = false;
            continue;
        }
        if (t.rfind("/*", 0) == 0 && t.find("*/") == std::string::npos) {
            inBlockComment = true;
            continue;
        }
        if (t.rfind("//", 0) == 0 || (t.rfind("#", 0) == 0 && lang.id != "c" && lang.id != "cpp" && lang.id != "markdown" && lang.id != "csharp") ||
            t.rfind("*", 0) == 0 || t.rfind("--", 0) == 0)
            continue;
        for (const auto& r : rules) {
            std::smatch m;
            if (!std::regex_search(line, m, r.re)) continue;
            Symbol s;
            s.line = static_cast<int>(i) + 1;
            s.kind = r.kind;
            s.name = sysutil::trim(m[static_cast<std::size_t>(r.nameGroup)].str());
            if (r.kind == "table" && m.size() > 1) s.kind = sysutil::lowerAscii(m[1].str());
            if (r.depthGroup) {
                std::size_t d = m[static_cast<std::size_t>(r.depthGroup)].length();
                s.depth = r.kind == "heading" ? static_cast<int>(d) - 1 : static_cast<int>(d / 4 + (d % 4 ? 1 : 0));
            } else {
                std::size_t indent = line.find_first_not_of(" \t");
                s.depth = indent == std::string::npos ? 0 : static_cast<int>(indent / 4);
            }
            if (controlWord(s.name) || s.name.empty()) continue;
            out.push_back(s);
            break;
        }
    }
    return out;
}

// ---- problems ---------------------------------------------------------------------------------

std::vector<Problem> parseProblems(const std::string& output, const fs::path& cwd) {
    static const std::regex gcc(R"(^(.+?):(\d+):(?:(\d+):)?\s*(fatal error|error|warning|note|Error|Warning):?\s*(.*)$)", std::regex::optimize);
    static const std::regex msvc(R"(^\s*(.+?)\((\d+)(?:,(\d+))?\)\s*:\s*(fatal error|error|warning|note)\s*(?:\w+\d+)?\s*:\s*(.*)$)", std::regex::optimize);
    static const std::regex rustLoc(R"(^\s*-->\s*(.+?):(\d+):(\d+))", std::regex::optimize);
    static const std::regex rustHead(R"(^(error|warning)(?:\[\w+\])?:\s*(.*)$)", std::regex::optimize);
    static const std::regex python(R"re(^\s*File "(.+?)", line (\d+))re", std::regex::optimize);
    static const std::regex pyError(R"(^(\w*(?:Error|Exception|Warning|Interrupt)\b.*)$)", std::regex::optimize);
    static const std::regex node(R"(^\s*at .*?\(?((?:[A-Za-z]:)?[^():]+):(\d+):(\d+)\)?$)", std::regex::optimize);
    static const std::regex plainLoc(R"(^((?:[A-Za-z]:)?[^\s:][^:]*\.\w+):(\d+):(\d+):?\s*(.*)$)", std::regex::optimize);
    static const std::regex lua(R"(^(?:lua\w*:\s*)?(.+?\.lua):(\d+):\s*(.*)$)", std::regex::optimize);
    std::vector<Problem> out;
    auto lines = sysutil::splitLines(output);
    std::string pendingRust, pendingRustSeverity;
    std::vector<std::size_t> pythonFrames;
    auto absolute = [&](const std::string& f) {
        fs::path p = sysutil::toPath(f);
        if (p.is_relative() && !cwd.empty()) p = cwd / p;
        return sysutil::fromPath(p.lexically_normal());
    };
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.size() > 4000) continue;
        std::smatch m;
        if ((line.rfind("error", 0) == 0 || line.rfind("warning", 0) == 0) && std::regex_match(line, m, rustHead)) {
            pendingRustSeverity = m[1];
            pendingRust = m[2];
            continue;
        }
        if (std::regex_search(line, m, rustLoc) && !pendingRust.empty()) {
            out.push_back({absolute(m[1]), std::stoi(m[2]), std::stoi(m[3]), pendingRustSeverity, pendingRust});
            pendingRust.clear();
            continue;
        }
        if (std::regex_match(line, m, msvc)) {
            std::string sev = m[4];
            if (sev == "fatal error") sev = "error";
            out.push_back({absolute(m[1]), std::stoi(m[2]), m[3].matched ? std::stoi(m[3]) : 0, sev, m[5]});
            continue;
        }
        if (std::regex_match(line, m, gcc) && m[1].str().find(' ') == std::string::npos) {
            std::string sev = sysutil::lowerAscii(m[4]);
            if (sev == "fatal error") sev = "error";
            out.push_back({absolute(m[1]), std::stoi(m[2]), m[3].matched ? std::stoi(m[3]) : 0, sev, m[5]});
            continue;
        }
        if (std::regex_search(line, m, python)) {
            out.push_back({absolute(m[1]), std::stoi(m[2]), 0, "error", ""});
            pythonFrames.push_back(out.size() - 1);
            continue;
        }
        if (!pythonFrames.empty() && std::regex_match(line, m, pyError)) {
            for (std::size_t f : pythonFrames) out[f].message = m[1];
            pythonFrames.clear();
            continue;
        }
        if (std::regex_match(line, m, lua)) {
            out.push_back({absolute(m[1]), std::stoi(m[2]), 0, "error", m[3]});
            continue;
        }
        if (std::regex_match(line, m, node) && m[1].str().find("node:") == std::string::npos && m[1].str().find("internal/") == std::string::npos) {
            out.push_back({absolute(m[1]), std::stoi(m[2]), std::stoi(m[3]), "error", "(stack trace)"});
            continue;
        }
        if (std::regex_match(line, m, plainLoc) && m[1].str().find(' ') == std::string::npos) {  // go vet, eslint unix, tsc --pretty false
            out.push_back({absolute(m[1]), std::stoi(m[2]), std::stoi(m[3]), "error", m[4]});
        }
    }
    // Python prints the innermost frame last; show that one first
    return out;
}

}  // namespace wetwaresuite::code
