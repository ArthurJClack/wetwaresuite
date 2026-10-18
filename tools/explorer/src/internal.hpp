// Shared helpers for the explorer.* commands.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/explorer/explorer.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

namespace wetwaresuite::explorer::detail {

using std::int64_t;

inline ParamSpec opt(const std::string& name, ParamType type, const std::string& what, Value def = {}) {
    return {name, type, what, false, {}, std::move(def)};
}
inline ParamSpec req(const std::string& name, ParamType type, const std::string& what) { return {name, type, what, true, {}, {}}; }
inline ParamSpec pathsParam(const std::string& what = "files or folders: one, or several separated by new lines or |") {
    return req("paths", ParamType::String, what);
}

inline Result guard(const std::function<Result()>& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        return Result::failure(e.what());
    }
}

inline ActionFactory reader(std::function<Result(const Params&)> fn) {
    return [fn](const Params& p) -> std::unique_ptr<Action> {
        return std::make_unique<FunctionAction>([fn, p] { return guard([&] { return fn(p); }); });
    };
}

using UndoList = std::vector<std::function<void()>>;

// A change to files that writes down how to take itself back: undo runs those
// steps newest first (moving anything it made to the trash, never deleting).
class FileChange : public Action {
public:
    using Fn = std::function<Result(const Params&, UndoList&)>;
    FileChange(Params p, Fn fn) : p_(std::move(p)), fn_(std::move(fn)) {}
    Result apply() override {
        undo_.clear();
        Result r = guard([&] { return fn_(p_, undo_); });
        if (!r.ok) revert();  // whatever got done before the failure is put back
        return r;
    }
    void revert() override {
        for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) {
            try {
                (*it)();
            } catch (const std::exception&) {
            }
        }
        undo_.clear();
    }

private:
    Params p_;
    Fn fn_;
    UndoList undo_;
};

inline ActionFactory changer(FileChange::Fn fn) {
    return [fn](const Params& p) -> std::unique_ptr<Action> { return std::make_unique<FileChange>(p, fn); };
}

std::vector<fs::path> splitPaths(const std::string& text);
fs::path pathParam(const Params& p, const std::string& key);
bool wildcard(const std::string& pattern, const std::string& name);
std::string entryRow(const Entry& e);

void registerFileCommands(Session& s);
void registerChangeCommands(Session& s);
void registerArchiveCommands(Session& s);
void registerToolCommands(Session& s);

}  // namespace wetwaresuite::explorer::detail
