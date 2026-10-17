// code.* commands that run things: a file, a project build, formatters, and the
// background jobs they become.
#include <algorithm>
#include <thread>

#include "internal.hpp"

namespace wetwaresuite::code::detail {

namespace {

using std::int64_t;

std::vector<ParamSpec> waitParams() {
    return {opt("wait", ParamType::Bool, "wait for it to finish (false = return at once with a job id; code.job_output follows it)", true),
            opt("timeout", ParamType::Number, "with wait=true, stop waiting after this many seconds (it keeps running as a job)", 60.0)};
}

Result follow(CodeState& st, std::shared_ptr<Job> job, const Params& p) {
    const bool wait = boolParam(p, "wait", true);
    const double timeout = numberParam(p, "timeout", 60.0);
    const auto start = std::chrono::steady_clock::now();
    pump(*job);
    while (wait && !job->done && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        pump(*job);
    }
    (void)st;
    Result r = jobResult(*job, false);
    if (job->done && job->exitCode != 0 && !job->stopped) r.ok = false;
    return r;
}

// the file to run for an open document: saved first when it has a path, else a temporary copy
fs::path fileFor(CodeDoc& doc, std::string& note) {
    if (!doc.path.empty()) {
        if (doc.modified()) {
            sysutil::writeFile(sysutil::toPath(doc.path), saveText(doc));
            doc.saved = doc.text;
            note = "saved " + doc.path + "\n";
        }
        return sysutil::toPath(doc.path);
    }
    const Language* lang = languageById(doc.language);
    std::string ext = lang && !lang->extensions.empty() ? lang->extensions.front() : ".txt";
    fs::path dir = fs::temp_directory_path() / "wetwaresuite-run";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path file = dir / ("untitled" + ext);
    sysutil::writeFile(file, saveText(doc));
    return file;
}

struct Formatter {
    std::vector<std::string> args;
};

// a formatter that is installed for this language, reading stdin and writing stdout
Formatter formatterFor(const std::string& lang, const std::string& name) {
    auto have = [](const char* p) { return findProgram(p).has_value(); };
    const std::string fileName = name.empty() ? "file" : name;
    if ((lang == "c" || lang == "cpp" || lang == "java" || lang == "csharp" || lang == "objc" || lang == "proto") && have("clang-format"))
        return {{"clang-format", "--assume-filename=" + fileName}};
    if (lang == "rust" && have("rustfmt")) return {{"rustfmt", "--emit", "stdout", "--quiet"}};
    if (lang == "go" && have("gofmt")) return {{"gofmt"}};
    if (lang == "python") {
        if (have("ruff")) return {{"ruff", "format", "-q", "-"}};
        if (have("black")) return {{"black", "-q", "-"}};
    }
    if (lang == "shell" && have("shfmt")) return {{"shfmt"}};
    if (lang == "lua" && have("stylua")) return {{"stylua", "-"}};
    if (lang == "zig" && have("zig")) return {{"zig", "fmt", "--stdin"}};
    static const std::set<std::string> pretty = {"javascript", "typescript", "css", "scss", "html", "json", "yaml", "markdown", "vue"};
    if (pretty.count(lang) && have("prettier")) return {{"prettier", "--stdin-filepath", fileName}};
    if ((lang == "javascript" || lang == "typescript" || lang == "c" || lang == "cpp") && have("clang-format"))
        return {{"clang-format", "--assume-filename=" + fileName}};
    return {};
}

void registerJobCommands(Session& s, StatePtr st) {
    auto& reg = s.registry();
    reg.add({"code.job_output", "Job output",
             "The output so far of a run or build (job=, default the newest), whether it is still running, its exit code and "
             "the errors and warnings found in it (file, line, column, severity, message).",
             {opt("job", ParamType::String, "job id, e.g. job1 (default: the newest)"), opt("full", ParamType::Bool, "all of the output, not just the end", false)},
             false, true},
            reader([st](const Params& p) { return jobResult(*findJob(*st, stringParam(p, "job")), boolParam(p, "full", false)); }));

    reg.add({"code.job_input", "Send input", "Type a line into a running program (for programs that ask questions).",
             {opt("job", ParamType::String, "job id (default: the newest)"), {"text", ParamType::String, "what to type", true},
              opt("newline", ParamType::Bool, "press Enter after it", true), opt("close", ParamType::Bool, "then close its input (end of file)", false)},
             false},
            reader([st](const Params& p) {
                auto job = findJob(*st, stringParam(p, "job"));
                pump(*job);
                if (job->done || !job->proc) throw std::runtime_error(job->id + " has finished");
                std::string t = stringParam(p, "text") + (boolParam(p, "newline", true) ? "\n" : "");
                if (!job->proc->write(t)) throw std::runtime_error("it is not reading input");
                if (boolParam(p, "close", false)) job->proc->closeInput();
                job->output += t;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                return jobResult(*job, false);
            }));

    reg.add({"code.job_stop", "Stop", "Stop a running program or build, and everything it started.",
             {opt("job", ParamType::String, "job id (default: the newest)")}, false},
            reader([st](const Params& p) {
                auto job = findJob(*st, stringParam(p, "job"));
                pump(*job);
                if (job->done) return Result::success(job->id + " had already finished").with("job", job->id);
                job->stopped = true;
                job->proc->kill();
                std::string rest;
                job->proc->wait(2, rest);
                job->output += rest;
                pump(*job);
                return Result::success("stopped " + job->id).with("job", job->id);
            }));

    reg.add({"code.jobs", "Jobs", "Runs and builds of this session: id, what it is, running or its exit code.", {}, false, true},
            reader([st](const Params&) {
                std::string text, rows;
                for (auto& [id, job] : st->jobs) {
                    pump(*job);
                    std::string state = !job->done ? "running" : job->stopped ? "stopped" : "exit " + std::to_string(job->exitCode);
                    text += id + ": " + job->description + " (" + state + ")\n";
                    rows += id + "\t" + state + "\t" + job->description + "\n";
                }
                if (st->jobs.empty()) text = "nothing has been run yet\n";
                return Result::success(text).with("count", static_cast<int64_t>(st->jobs.size())).with("rows", rows);
            }));

    reg.add({"code.errors", "Errors in output",
             "Find compiler and runtime errors in a job's output (job=) or any text (text=): file, line, column, severity, message.",
             {opt("job", ParamType::String, "job id (default: the newest)"), opt("text", ParamType::String, "or parse this text"),
              opt("folder", ParamType::String, "folder that relative file names are in")},
             false, true},
            reader([st](const Params& p) {
                std::string text = stringParam(p, "text");
                fs::path cwd = sysutil::toPath(stringParam(p, "folder"));
                if (text.empty()) {
                    auto job = findJob(*st, stringParam(p, "job"));
                    pump(*job);
                    text = job->output;
                    if (cwd.empty()) cwd = job->cwd;
                }
                auto problems = parseProblems(text, cwd);
                std::string out;
                for (const auto& pr : problems)
                    out += pr.file + ":" + std::to_string(pr.line) + (pr.col ? ":" + std::to_string(pr.col) : "") + ": " + pr.severity + ": " + pr.message + "\n";
                if (problems.empty()) out = "no errors or warnings found\n";
                return Result::success(out).with("count", static_cast<int64_t>(problems.size())).with("rows", problemRows(problems));
            }));
}

}  // namespace

void registerRunCommands(Session& s, StatePtr st) {
    auto& reg = s.registry();
    reg.add({"code.toolchains", "Installed toolchains",
             "The compilers, interpreters and build tools found on this computer (nothing is ever downloaded).", {}, false, true},
            reader([](const Params&) {
                auto tcs = findToolchains();
                std::string text, rows;
                for (const auto& t : tcs) {
                    text += t.name + " - " + t.languages + " (" + t.program + ")\n";
                    rows += t.name + "\t" + t.languages + "\t" + t.program + "\n";
                }
                if (tcs.empty()) text = "no compilers or interpreters found on PATH\n";
                return Result::success(text).with("count", static_cast<int64_t>(tcs.size())).with("rows", rows);
            }));

    std::vector<ParamSpec> runParams = {opt("doc", ParamType::String, "an open file (saved first if it has a path)"),
                                        opt("path", ParamType::String, "or a file on disk"),
                                        opt("args", ParamType::String, "arguments for the program, e.g. \"-v input.txt\""),
                                        opt("input", ParamType::String, "text for the program's input (then end of file)")};
    for (auto& w : waitParams()) runParams.push_back(w);
    reg.add({"code.run", "Run file",
             "Run a file with the compiler or interpreter installed for its language: C/C++ (gcc, clang, MSVC), Rust, Go, "
             "Python, JavaScript/TypeScript (node, deno, bun), Java, C#, Lua, Ruby, PHP, Perl, shell, PowerShell, SQL "
             "(sqlite3) and more. Output, exit code and parsed errors come back; long runs continue as a job.",
             runParams, false},
            [&s, st](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&s, st, p] {
                    return guard([&] {
                        std::string note;
                        fs::path file;
                        const Language* lang = nullptr;
                        if (!stringParam(p, "doc").empty()) {
                            auto doc = needDoc(s, stringParam(p, "doc"));
                            file = fileFor(*doc, note);
                            lang = languageById(doc->language);
                        } else {
                            file = sysutil::toPath(stringParam(p, "path"));
                            std::error_code ec;
                            if (file.empty() || !fs::is_regular_file(file, ec)) throw std::runtime_error("give doc= or the path= of a file to run");
                            file = fs::absolute(file, ec);
                        }
                        if (!lang) lang = &detectLanguage(sysutil::nameOf(file), sysutil::readFile(file).substr(0, 200));
                        Plan plan = planRun(file, *lang, splitArgs(stringParam(p, "args")));
                        if (p.count("input") && !plan.steps.empty() && !plan.steps.back().hasInput) {
                            plan.steps.back().input = stringParam(p, "input");
                            plan.steps.back().hasInput = true;
                        }
                        std::string error;
                        auto job = startJob(*st, std::move(plan), error);
                        if (!job) throw std::runtime_error(error);
                        Result r = follow(*st, job, p);
                        if (!note.empty()) r.message = note + r.message;
                        return r;
                    });
                });
            });

    std::vector<ParamSpec> buildParams = {{"action", ParamType::String, "what to do", false, {"build", "test", "run", "clean"}, std::string("build")},
                                          opt("folder", ParamType::String, "project folder (default: the open project)")};
    for (auto& w : waitParams()) buildParams.push_back(w);
    reg.add({"code.build", "Build project",
             "Build, test, run or clean a project folder with its own build system: CMake, Cargo, Go, npm/pnpm/yarn/bun, "
             "Make, Meson, .NET, Maven, Gradle, Zig, pytest. Errors and warnings are parsed into file:line rows.",
             buildParams, false},
            reader([st](const Params& p) {
                fs::path folder = folderParam(*st, p);
                std::error_code ec;
                fs::path root = projectKind(folder).empty() ? findProjectRoot(fs::absolute(folder, ec)) : folder;
                if (root.empty()) root = folder;
                Plan plan = planProject(root, stringParam(p, "action", "build"));
                std::string error;
                auto job = startJob(*st, std::move(plan), error);
                if (!job) throw std::runtime_error(error);
                return follow(*st, job, p);
            }));

    reg.add({"code.format", "Format file",
             "Tidy an open file with the formatter installed for its language (clang-format, rustfmt, gofmt, ruff or black, "
             "prettier, shfmt, stylua, zig fmt). Undoable.",
             {docParam()}, true},
            docEditor(s, [](CodeDoc& doc, const Params&) {
                Formatter f = formatterFor(doc.language, doc.path.empty() ? std::string() : sysutil::nameOf(sysutil::toPath(doc.path)));
                if (f.args.empty()) throw std::runtime_error("no formatter for " + doc.language + " is installed (clang-format, rustfmt, gofmt, black, prettier, shfmt, stylua)");
                ChildProcess proc;
                std::string error, out;
                std::error_code ec;
                if (!proc.start(f.args, doc.path.empty() ? fs::current_path(ec) : sysutil::toPath(doc.path).parent_path(), error)) throw std::runtime_error(error);
                proc.write(doc.text);
                proc.closeInput();
                if (!proc.wait(30, out)) {
                    proc.kill();
                    throw std::runtime_error(f.args[0] + " took too long");
                }
                if (proc.exitCode() != 0) throw std::runtime_error(f.args[0] + " failed:\n" + out);
                out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
                if (!doc.text.empty() && doc.text.back() != '\n' && !out.empty() && out.back() == '\n') out.pop_back();
                if (out == doc.text) return Result::success("already tidy (" + f.args[0] + ")");
                doc.text = out;
                return Result::success("formatted with " + f.args[0]).with("lines", static_cast<int64_t>(lines(doc.text).size()));
            }));

    registerJobCommands(s, st);
}

}  // namespace wetwaresuite::code::detail
