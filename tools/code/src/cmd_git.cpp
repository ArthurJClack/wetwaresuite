// code.git_* commands: status, diff, stage, commit, log and branches through the
// git program when it is installed. Nothing talks to a server (no push or pull):
// the suite stays offline.
#include "internal.hpp"

namespace wetwaresuite::code::detail {

namespace {

using std::int64_t;

ProcessResult git(const fs::path& dir, std::vector<std::string> args) {
    if (!findProgram("git")) throw std::runtime_error("git is not installed (get it from git-scm.com or your package manager)");
    args.insert(args.begin(), {"git", "-C", sysutil::fromPath(dir), "-c", "color.ui=never", "-c", "core.quotepath=off"});
    return runProgram(args);
}

ProcessResult gitOk(const fs::path& dir, std::vector<std::string> args) {
    ProcessResult r = git(dir, std::move(args));
    if (r.exitCode != 0) {
        std::string out = sysutil::trim(r.output);
        if (out.find("not a git repository") != std::string::npos) throw std::runtime_error(sysutil::fromPath(dir) + " is not a git repository (code.git_init makes one)");
        throw std::runtime_error(out.empty() ? "git failed" : out);
    }
    return r;
}

std::vector<std::string> pathList(const Params& p) {
    std::vector<std::string> out;
    for (auto& a : splitArgs(stringParam(p, "paths", "."))) out.push_back(a);
    if (out.empty()) out.push_back(".");
    return out;
}

std::string stateName(char c) {
    switch (c) {
        case 'M': return "modified";
        case 'A': return "added";
        case 'D': return "deleted";
        case 'R': return "renamed";
        case 'C': return "copied";
        case 'U': return "conflict";
        case '?': return "new";
        case '!': return "ignored";
        default: return "";
    }
}

ParamSpec folderOpt() { return opt("folder", ParamType::String, "the repository folder (default: the open project)"); }

void registerReadCommands(Session& s, StatePtr st) {
    auto& reg = s.registry();
    reg.add({"code.git_status", "Changes",
             "The branch and every changed file: staged (ready to commit) and not staged, new files, conflicts.",
             {folderOpt()}, false, true},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                ProcessResult r = gitOk(dir, {"status", "--porcelain=v1", "-b", "-uall"});
                std::string branch, text, rows;
                std::size_t staged = 0, unstaged = 0;
                for (const auto& line : lines(r.output)) {
                    if (line.size() < 3) continue;
                    if (line.rfind("## ", 0) == 0) {
                        branch = line.substr(3);
                        continue;
                    }
                    const char x = line[0], y = line[1];
                    std::string path = line.substr(3);
                    if (x == '?') {
                        ++unstaged;
                        text += "  new (not staged): " + path + "\n";
                        rows += "?\t?\t" + path + "\n";
                        continue;
                    }
                    if (x != ' ') {
                        ++staged;
                        text += "  " + stateName(x) + " (staged): " + path + "\n";
                    }
                    if (y != ' ') {
                        ++unstaged;
                        text += "  " + stateName(y) + " (not staged): " + path + "\n";
                    }
                    rows += std::string(1, x) + "\t" + std::string(1, y) + "\t" + path + "\n";
                }
                std::string head = "branch " + (branch.empty() ? "?" : branch) + "\n";
                if (!staged && !unstaged) head += "nothing changed\n";
                return Result::success(head + text)
                    .with("branch", branch.substr(0, branch.find("...")))
                    .with("staged", static_cast<int64_t>(staged))
                    .with("unstaged", static_cast<int64_t>(unstaged))
                    .with("rows", rows);
            }));

    reg.add({"code.git_diff", "Diff",
             "What changed, as a unified diff: of everything or one file, not yet staged (default) or staged.",
             {folderOpt(), opt("file", ParamType::String, "only this file"), opt("staged", ParamType::Bool, "the staged changes", false),
              opt("commit", ParamType::String, "show this commit instead (e.g. HEAD~1, a hash)")},
             false, true},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                std::vector<std::string> args;
                if (!stringParam(p, "commit").empty()) args = {"show", "--stat", "-p", stringParam(p, "commit")};
                else args = {"diff"};
                if (boolParam(p, "staged", false) && args[0] == "diff") args.push_back("--cached");
                if (!stringParam(p, "file").empty()) {
                    args.push_back("--");
                    args.push_back(stringParam(p, "file"));
                }
                ProcessResult r = gitOk(dir, args);
                std::size_t added = 0, removed = 0;
                for (const auto& l : lines(r.output)) {
                    if (l.rfind("+++", 0) == 0 || l.rfind("---", 0) == 0) continue;
                    if (!l.empty() && l[0] == '+') ++added;
                    if (!l.empty() && l[0] == '-') ++removed;
                }
                return Result::success(r.output.empty() ? "no differences" : r.output)
                    .with("diff", r.output)
                    .with("added", static_cast<int64_t>(added))
                    .with("removed", static_cast<int64_t>(removed));
            }));

    reg.add({"code.git_log", "History", "Recent commits: short hash, author, when, and message.",
             {folderOpt(), opt("count", ParamType::Int, "how many", int64_t{20}), opt("file", ParamType::String, "only commits touching this file")},
             false, true},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                std::vector<std::string> args = {"log", "-n", std::to_string(std::max<int64_t>(1, intParam(p, "count", 20))), "--format=%h%x09%an%x09%ar%x09%s"};
                if (!stringParam(p, "file").empty()) args.insert(args.end(), {"--", stringParam(p, "file")});
                ProcessResult r = git(dir, args);
                if (r.exitCode != 0) {
                    if (r.output.find("does not have any commits") != std::string::npos) return Result::success("no commits yet").with("count", int64_t{0}).with("rows", "");
                    throw std::runtime_error(sysutil::trim(r.output));
                }
                std::string text;
                std::size_t n = 0;
                for (const auto& l : lines(r.output)) {
                    std::vector<std::string> f;
                    std::size_t start = 0;
                    while (true) {
                        std::size_t tab = l.find('\t', start);
                        f.push_back(l.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                        if (tab == std::string::npos) break;
                        start = tab + 1;
                    }
                    if (f.size() < 4) continue;
                    ++n;
                    text += f[0] + "  " + f[3] + "  (" + f[1] + ", " + f[2] + ")\n";
                }
                return Result::success(n ? text : "no commits yet").with("count", static_cast<int64_t>(n)).with("rows", r.output);
            }));

    reg.add({"code.git_branches", "Branches", "The branches, the current one marked with *.", {folderOpt()}, false, true},
            reader([st](const Params& p) {
                ProcessResult r = gitOk(folderParam(*st, p), {"branch", "--format=%(HEAD)%(refname:short)"});
                std::string text, current;
                std::size_t n = 0;
                for (const auto& l : lines(r.output)) {
                    if (l.size() < 2) continue;
                    ++n;
                    if (l[0] == '*') current = l.substr(1);
                    text += (l[0] == '*' ? "* " : "  ") + l.substr(1) + "\n";
                }
                return Result::success(n ? text : "no branches yet (make a commit first)").with("current", current).with("count", static_cast<int64_t>(n));
            }));
}

void registerWriteCommands(Session& s, StatePtr st) {
    auto& reg = s.registry();
    reg.add({"code.git_stage", "Stage", "Mark files to go in the next commit (paths=\".\" = everything).",
             {folderOpt(), opt("paths", ParamType::String, "files, separated by spaces (quotes for names with spaces)", std::string("."))}, false},
            reader([st](const Params& p) {
                auto paths = pathList(p);
                std::vector<std::string> args = {"add", "-A", "--"};
                args.insert(args.end(), paths.begin(), paths.end());
                gitOk(folderParam(*st, p), args);
                return Result::success("staged " + stringParam(p, "paths", "."));
            }));

    reg.add({"code.git_unstage", "Unstage", "Take files back out of the next commit (their changes stay).",
             {folderOpt(), opt("paths", ParamType::String, "files, separated by spaces", std::string("."))}, false},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                auto paths = pathList(p);
                std::vector<std::string> args = {"reset", "-q", "--"};
                ProcessResult head = git(dir, {"rev-parse", "--verify", "-q", "HEAD"});
                if (head.exitCode != 0) args = {"rm", "-q", "--cached", "-r", "--"};  // no commits yet
                args.insert(args.end(), paths.begin(), paths.end());
                gitOk(dir, args);
                return Result::success("unstaged " + stringParam(p, "paths", "."));
            }));

    reg.add({"code.git_commit", "Commit",
             "Save the staged changes as a commit with a message (all=true stages every changed file first).",
             {folderOpt(), {"message", ParamType::String, "what the change does", true}, opt("all", ParamType::Bool, "stage every change first", false)},
             false},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                const std::string msg = sysutil::trim(stringParam(p, "message"));
                if (msg.empty()) throw std::runtime_error("write a commit message");
                if (boolParam(p, "all", false)) gitOk(dir, {"add", "-A"});
                ProcessResult r = git(dir, {"commit", "-q", "-m", msg});
                if (r.exitCode != 0) {
                    std::string out = sysutil::trim(r.output);
                    if (out.find("user.email") != std::string::npos || out.find("Author identity") != std::string::npos)
                        throw std::runtime_error("git needs your name and email first: git config --global user.name \"Your Name\" and git config --global user.email you@example.com");
                    if (out.find("nothing to commit") != std::string::npos || out.find("no changes added") != std::string::npos)
                        throw std::runtime_error("nothing staged to commit (code.git_stage, or all=true)");
                    throw std::runtime_error(out);
                }
                ProcessResult h = git(dir, {"log", "-1", "--format=%h"});
                const std::string hash = sysutil::trim(h.output);
                return Result::success("committed " + hash + ": " + msg).with("commit", hash);
            }));

    reg.add({"code.git_switch", "Switch branch", "Switch to another branch, or make a new one (create=true).",
             {folderOpt(), {"branch", ParamType::String, "branch name", true}, opt("create", ParamType::Bool, "make a new branch here", false)}, false},
            reader([st](const Params& p) {
                const std::string b = stringParam(p, "branch");
                if (b.empty()) throw std::runtime_error("give a branch name");
                gitOk(folderParam(*st, p), boolParam(p, "create", false) ? std::vector<std::string>{"checkout", "-q", "-b", b} : std::vector<std::string>{"checkout", "-q", b});
                return Result::success("on branch " + b).with("branch", b);
            }));

    reg.add({"code.git_init", "Start a repository", "Make a folder a git repository so changes can be tracked.", {folderOpt()}, false},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                gitOk(dir, {"init", "-q"});
                return Result::success("git repository started in " + sysutil::fromPath(dir));
            }));
}

}  // namespace

void registerGitCommands(Session& s, StatePtr st) {
    registerReadCommands(s, st);
    registerWriteCommands(s, st);
}

}  // namespace wetwaresuite::code::detail
