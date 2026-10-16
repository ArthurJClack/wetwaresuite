// Running files and building projects with the toolchains already on this
// computer. Nothing is ever downloaded: when a compiler or interpreter is
// missing, the error says which one to install.
#include <fstream>
#include <stdexcept>

#include "wetwaresuite/code/code.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

namespace wetwaresuite::code {

namespace {

#ifdef _WIN32
constexpr const char* kExe = ".exe";
#else
constexpr const char* kExe = "";
#endif

std::string first(std::initializer_list<const char*> names) {
    for (const char* n : names) {
        if (findProgram(n)) return n;
    }
    return {};
}

std::string need(std::initializer_list<const char*> names, const std::string& what) {
    std::string p = first(names);
    if (p.empty()) {
        std::string list;
        for (const char* n : names) list += (list.empty() ? "" : ", ") + std::string(n);
        throw std::runtime_error("to run " + what + " install one of: " + list + " (none is on PATH)");
    }
    return p;
}

fs::path outputFor(const fs::path& file) {
    fs::path dir = fs::temp_directory_path() / "wetwaresuite-run";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / (sysutil::fromPath(file.stem()) + kExe);
}

std::vector<std::string> with(std::vector<std::string> a, const std::vector<std::string>& more) {
    a.insert(a.end(), more.begin(), more.end());
    return a;
}

// the project's own build wrapper script (gradlew, mvnw)
std::string wrapper(const char* name) {
#ifdef _WIN32
    return std::string(name) + (std::string(name) == "mvnw" ? ".cmd" : ".bat");
#else
    return name;
#endif
}

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

Plan compileThenRun(const std::string& compiler, std::vector<std::string> compileArgs, const fs::path& out, const fs::path& dir,
                    const std::vector<std::string>& args, const std::string& what) {
    Plan p;
    p.steps.push_back({std::move(compileArgs), compiler == "cl" ? out.parent_path() : dir, {}, false});
    p.steps.push_back({with({sysutil::fromPath(out)}, args), dir, {}, false});
    p.description = "compile with " + compiler + ", then run " + what;
    return p;
}

Plan cFamily(const fs::path& file, const std::string& lang, const std::vector<std::string>& args) {
    const bool cpp = lang == "cpp";
    std::string cc = cpp ? need({"c++", "g++", "clang++", "cl"}, "C++") : need({"cc", "gcc", "clang", "cl"}, "C");
    const fs::path out = outputFor(file), dir = file.parent_path();
    const std::string src = sysutil::fromPath(file);
    if (cc == "cl") {
        std::vector<std::string> c = {"cl", "/nologo", "/EHsc", "/utf-8", cpp ? "/std:c++20" : "/std:c17", src, "/Fe:" + sysutil::fromPath(out)};
        return compileThenRun(cc, c, out, dir, args, file.filename().string());
    }
    std::vector<std::string> c = {cc, cpp ? "-std=c++20" : "-std=c17", "-g", "-Wall", src, "-o", sysutil::fromPath(out)};
    if (!cpp) c.push_back("-lm");
    return compileThenRun(cc, c, out, dir, args, sysutil::nameOf(file));
}

// A compiled program (ELF, Windows .exe, Mach-O) or, outside Windows, a file
// marked executable that is not a script for a language we know.
bool isProgram(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    unsigned char m[4] = {0, 0, 0, 0};
    in.read(reinterpret_cast<char*>(m), 4);
    if (in.gcount() >= 2 && m[0] == 'M' && m[1] == 'Z') return true;
    if (in.gcount() < 4) return false;
    if (m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F') return true;
    const unsigned v = (unsigned(m[0]) << 24) | (unsigned(m[1]) << 16) | (unsigned(m[2]) << 8) | unsigned(m[3]);
    return v == 0xfeedfaceu || v == 0xfeedfacfu || v == 0xcefaedfeu || v == 0xcffaedfeu || v == 0xcafebabeu;
}

Plan interpreted(std::vector<std::string> cmd, const fs::path& file, const std::vector<std::string>& args, const std::string& what) {
    Plan p;
    cmd.push_back(sysutil::fromPath(file));
    p.steps.push_back({with(std::move(cmd), args), file.parent_path(), {}, false});
    p.description = "run with " + what;
    return p;
}

}  // namespace

std::vector<std::string> splitArgs(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool any = false;
    char quote = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quote) {
            if (c == quote) quote = 0;
            else if (c == '\\' && quote == '"' && i + 1 < text.size() && (text[i + 1] == '"' || text[i + 1] == '\\')) cur += text[++i];
            else cur += c;
        } else if (c == '"' || c == '\'') {
            quote = c;
            any = true;
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (any || !cur.empty()) out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += c;
        }
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}

std::vector<Toolchain> findToolchains() {
    struct Known {
        const char* name;
        const char* languages;
    };
    static const Known known[] = {
        {"gcc", "C"}, {"g++", "C++"}, {"clang", "C"}, {"clang++", "C++"}, {"cl", "C, C++ (MSVC)"}, {"cmake", "CMake projects"},
        {"make", "Makefiles"}, {"ninja", "Ninja builds"}, {"meson", "Meson projects"}, {"rustc", "Rust"}, {"cargo", "Rust projects"},
        {"go", "Go"}, {"python3", "Python"}, {"python", "Python"}, {"py", "Python (launcher)"}, {"node", "JavaScript"},
        {"deno", "JavaScript, TypeScript"}, {"bun", "JavaScript, TypeScript"}, {"tsc", "TypeScript compiler"}, {"tsx", "TypeScript"},
        {"npm", "npm projects"}, {"pnpm", "pnpm projects"}, {"yarn", "yarn projects"}, {"java", "Java"}, {"javac", "Java compiler"},
        {"mvn", "Maven projects"}, {"gradle", "Gradle projects"}, {"kotlinc", "Kotlin"}, {"dotnet", "C#, .NET"}, {"swift", "Swift"},
        {"dart", "Dart"}, {"scala", "Scala"}, {"lua", "Lua"}, {"luajit", "Lua"}, {"ruby", "Ruby"}, {"perl", "Perl"}, {"php", "PHP"},
        {"Rscript", "R"}, {"julia", "Julia"}, {"elixir", "Elixir"}, {"runghc", "Haskell"}, {"ghc", "Haskell"}, {"zig", "Zig"},
        {"bash", "Shell"}, {"sh", "Shell"}, {"zsh", "Shell"}, {"fish", "Fish"}, {"pwsh", "PowerShell"}, {"powershell", "PowerShell"},
        {"sqlite3", "SQL (SQLite)"}, {"git", "version control"}, {"clang-format", "C/C++ formatting"}, {"rustfmt", "Rust formatting"},
        {"gofmt", "Go formatting"}, {"black", "Python formatting"}, {"prettier", "JS/TS/CSS/HTML formatting"},
    };
    std::vector<Toolchain> out;
    for (const auto& k : known) {
        if (auto p = findProgram(k.name)) out.push_back({k.name, sysutil::fromPath(*p), k.languages});
    }
    return out;
}

Plan planRun(const fs::path& file, const Language& lang, const std::vector<std::string>& args) {
    const std::string id = lang.id;
    const std::string ext = sysutil::extensionOf(file);
    if ((id.empty() || id == "text") && isProgram(file)) {
        Plan p;
        p.steps.push_back({with({sysutil::fromPath(fs::absolute(file))}, args), file.parent_path(), {}, false});
        p.description = "run the program";
        return p;
    }
    if (id == "c" || id == "cpp") return cFamily(file, id, args);
    if (id == "rust") {
        need({"rustc"}, "Rust");
        fs::path out = outputFor(file);
        return compileThenRun("rustc", {"rustc", "--edition", "2021", "-g", sysutil::fromPath(file), "-o", sysutil::fromPath(out)}, out, file.parent_path(), args, sysutil::nameOf(file));
    }
    if (id == "go") return interpreted({need({"go"}, "Go"), "run"}, file, args, "go run");
    if (id == "python") {
        std::string py = need({"python3", "python", "py"}, "Python");
        return interpreted(py == "py" ? std::vector<std::string>{"py", "-3", "-u"} : std::vector<std::string>{py, "-u"}, file, args, py);
    }
    if (id == "javascript") {
        std::string js = need({"node", "bun", "deno"}, "JavaScript");
        return interpreted(js == "deno" ? std::vector<std::string>{"deno", "run", "-A"} : js == "bun" ? std::vector<std::string>{"bun", "run"} : std::vector<std::string>{js}, file, args, js);
    }
    if (id == "typescript") {
        std::string ts = need({"deno", "bun", "tsx", "ts-node", "node"}, "TypeScript");
        if (ts == "deno") return interpreted({"deno", "run", "-A"}, file, args, "deno");
        if (ts == "bun") return interpreted({"bun", "run"}, file, args, "bun");
        if (ts == "node") return interpreted({"node", "--experimental-strip-types"}, file, args, "node (type stripping, Node 22.6+)");
        return interpreted({ts}, file, args, ts);
    }
    if (id == "java") return interpreted({need({"java"}, "Java (JDK 11 or newer)")}, file, args, "java");
    if (id == "kotlin") {
        std::string k = need({"kotlinc", "kotlin"}, "Kotlin");
        if (ext == ".kts") return interpreted({k, "-script"}, file, args, "kotlinc -script");
        fs::path jar = outputFor(file);
        jar.replace_extension(".jar");
        Plan p;
        p.steps.push_back({{k, sysutil::fromPath(file), "-include-runtime", "-d", sysutil::fromPath(jar)}, file.parent_path(), {}, false});
        p.steps.push_back({with({need({"java"}, "Kotlin programs (java)"), "-jar", sysutil::fromPath(jar)}, args), file.parent_path(), {}, false});
        p.description = "compile with kotlinc, then run with java";
        return p;
    }
    if (id == "csharp") {
        fs::path root = findProjectRoot(file);
        if (!root.empty() && projectKind(root) == "dotnet") return planProject(root, "run");
        return interpreted({need({"dotnet"}, "C# (.NET 10 runs single files)"), "run"}, file, args, "dotnet run");
    }
    if (id == "swift") return interpreted({need({"swift"}, "Swift")}, file, args, "swift");
    if (id == "dart") return interpreted({need({"dart"}, "Dart"), "run"}, file, args, "dart run");
    if (id == "scala") return interpreted({need({"scala", "scala-cli"}, "Scala")}, file, args, "scala");
    if (id == "lua") return interpreted({need({"lua", "luajit", "lua5.4", "lua5.3"}, "Lua")}, file, args, "lua");
    if (id == "ruby") return interpreted({need({"ruby"}, "Ruby")}, file, args, "ruby");
    if (id == "perl") return interpreted({need({"perl"}, "Perl")}, file, args, "perl");
    if (id == "php") return interpreted({need({"php"}, "PHP")}, file, args, "php");
    if (id == "r") return interpreted({need({"Rscript"}, "R")}, file, args, "Rscript");
    if (id == "julia") return interpreted({need({"julia"}, "Julia")}, file, args, "julia");
    if (id == "elixir") return interpreted({need({"elixir"}, "Elixir")}, file, args, "elixir");
    if (id == "haskell") return interpreted({need({"runghc", "runhaskell"}, "Haskell")}, file, args, "runghc");
    if (id == "zig") return interpreted({need({"zig"}, "Zig"), "run"}, file, args, "zig run");
    if (id == "shell") {
        std::string sh = ext == ".zsh" ? need({"zsh", "bash", "sh"}, "zsh scripts") : need({"bash", "sh"}, "shell scripts");
        return interpreted({sh}, file, args, sh);
    }
    if (id == "fish") return interpreted({need({"fish"}, "fish scripts")}, file, args, "fish");
    if (id == "powershell") {
        std::string ps = need({"pwsh", "powershell"}, "PowerShell");
        return interpreted({ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File"}, file, args, ps);
    }
    if (id == "batch") return interpreted({need({"cmd"}, "batch files (Windows)"), "/d", "/c"}, file, args, "cmd");
    if (id == "makefile") return interpreted({need({"make"}, "Makefiles"), "-f"}, file, args, "make");
    if (id == "sql") {
        Plan p;
        p.steps.push_back({{need({"sqlite3"}, "SQL files (SQLite)"), ":memory:"}, file.parent_path(), sysutil::readFile(file), true});
        p.description = "run in an in-memory SQLite database";
        return p;
    }
    if (id == "cmake") return planProject(file.parent_path(), "build");
    throw std::runtime_error(lang.name + " files can't be run here" + std::string(id == "html" ? "; open it in a browser instead (explorer.launch)" : ""));
}

std::string projectKind(const fs::path& folder) {
    struct Marker {
        const char* file;
        const char* kind;
    };
    static const Marker markers[] = {{"Cargo.toml", "cargo"}, {"go.mod", "go"}, {"CMakeLists.txt", "cmake"}, {"meson.build", "meson"},
                                     {"package.json", "npm"}, {"pom.xml", "maven"}, {"build.gradle", "gradle"},
                                     {"build.gradle.kts", "gradle"}, {"Makefile", "make"}, {"makefile", "make"}, {"GNUmakefile", "make"},
                                     {"pyproject.toml", "python"}, {"setup.py", "python"}, {"build.zig", "zig"}};
    for (const auto& m : markers) {
        if (fileExists(folder / m.file)) return m.kind;
    }
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec)) {
        std::string ext = sysutil::extensionOf(e.path());
        if (ext == ".sln" || ext == ".csproj" || ext == ".fsproj") return "dotnet";
    }
    return {};
}

fs::path findProjectRoot(const fs::path& file) {
    std::error_code ec;
    fs::path dir = fs::is_directory(file, ec) ? file : file.parent_path();
    fs::path git;
    for (fs::path d = dir; !d.empty(); d = d.parent_path()) {
        if (!projectKind(d).empty()) return d;
        if (git.empty() && fileExists(d / ".git")) git = d;
        if (d == d.parent_path()) break;
    }
    return git;
}

Plan planProject(const fs::path& folder, const std::string& action) {
    const std::string kind = projectKind(folder);
    Plan p;
    auto step = [&](std::vector<std::string> args) { p.steps.push_back({std::move(args), folder, {}, false}); };
    if (kind.empty()) throw std::runtime_error("no build file in " + sysutil::fromPath(folder) + " (looked for CMakeLists.txt, Cargo.toml, go.mod, package.json, Makefile, *.csproj, pom.xml, build.gradle, meson.build)");
    if (kind == "cargo") {
        std::string c = need({"cargo"}, "Rust projects");
        step({c, action == "clean" ? "clean" : action == "test" ? "test" : action == "run" ? "run" : "build"});
    } else if (kind == "go") {
        std::string g = need({"go"}, "Go projects");
        if (action == "run") step({g, "run", "."});
        else if (action == "test") step({g, "test", "./..."});
        else if (action == "clean") step({g, "clean"});
        else step({g, "build", "./..."});
    } else if (kind == "cmake") {
        std::string c = need({"cmake"}, "CMake projects");
        if (action == "clean") {
            step({c, "--build", "build", "--target", "clean"});
        } else {
            if (!fileExists(folder / "build" / "CMakeCache.txt")) step({c, "-S", ".", "-B", "build"});
            step({c, "--build", "build", "--parallel"});
            if (action == "test") step({need({"ctest"}, "CMake tests"), "--test-dir", "build", "--output-on-failure"});
        }
    } else if (kind == "meson") {
        std::string m = need({"meson"}, "Meson projects");
        if (!fileExists(folder / "build")) step({m, "setup", "build"});
        step({m, action == "test" ? "test" : "compile", "-C", "build"});
    } else if (kind == "npm") {
        std::string tool = fileExists(folder / "pnpm-lock.yaml") ? need({"pnpm"}, "pnpm projects")
                           : fileExists(folder / "yarn.lock")    ? need({"yarn"}, "yarn projects")
                           : fileExists(folder / "bun.lockb")    ? need({"bun"}, "bun projects")
                                                             : need({"npm"}, "npm projects");
        std::string script = action == "run" ? "start" : action;
        step({tool, "run", script});
    } else if (kind == "maven") {
        std::string m = fileExists(folder / wrapper("mvnw")) ? sysutil::fromPath(folder / wrapper("mvnw")) : need({"mvn"}, "Maven projects");
        step({m, "-q", action == "clean" ? "clean" : action == "test" ? "test" : "package"});
    } else if (kind == "gradle") {
        std::string g = fileExists(folder / wrapper("gradlew")) ? sysutil::fromPath(folder / wrapper("gradlew")) : need({"gradle"}, "Gradle projects");
        step({g, action == "clean" ? "clean" : action == "test" ? "test" : action == "run" ? "run" : "build"});
    } else if (kind == "dotnet") {
        step({need({"dotnet"}, ".NET projects"), action == "clean" ? "clean" : action == "test" ? "test" : action == "run" ? "run" : "build"});
    } else if (kind == "make") {
        std::string m = need({"make", "gmake", "mingw32-make"}, "Makefiles");
        if (action == "build") step({m});
        else step({m, action});
    } else if (kind == "python") {
        std::string py = need({"python3", "python", "py"}, "Python projects");
        if (action == "test") step({py, "-m", "pytest"});
        else throw std::runtime_error("Python projects have no build step here; run a file instead, or action=test for pytest");
    } else if (kind == "zig") {
        step({need({"zig"}, "Zig projects"), "build", action == "test" ? "test" : action == "run" ? "run" : "install"});
    }
    p.description = action + " the " + kind + " project";
    return p;
}

}  // namespace wetwaresuite::code
