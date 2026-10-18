// The command system every tool is built on. Every user action is a named
// command with typed parameters, so the same action can come from a button,
// the `wetwaresuite` CLI, a batch script or an MCP client.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "wetwaresuite/core/workspace.hpp"

namespace wetwaresuite {

using NumberList = std::vector<double>;
using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string, NumberList>;
using Params = std::map<std::string, Value>;

enum class ParamType { Bool, Int, Number, String, NumberList };

const char* toString(ParamType type);

struct ParamSpec {
    std::string name;
    ParamType type = ParamType::String;
    std::string description;
    bool required = true;
    std::vector<std::string> choices;  // allowed values for a String param, empty = any
    Value defaultValue;                // shown to clients; the action applies it
};

struct CommandInfo {
    std::string id;           // dotted, e.g. "photo.resize"
    std::string title;        // shown in menus and as the undo label
    std::string description;  // shown to people and to MCP clients
    std::vector<ParamSpec> params;
    bool undoable = false;    // read-only and export commands are not
    bool readOnly = false;    // changes nothing outside the session (no files written, nothing discarded)
};

// Ordered key/value output, e.g. {"doc": "img1"}, {"width": 800}.
using Fields = std::vector<std::pair<std::string, Value>>;

struct Result {
    bool ok = true;
    std::string message;
    Value value;
    Fields data;                          // structured output for scripts and MCP
    std::vector<Attachment> attachments;  // previews, rendered files

    static Result success(std::string message, Value value = {}) {
        Result r;
        r.message = std::move(message);
        r.value = std::move(value);
        return r;
    }
    static Result failure(std::string message) {
        Result r;
        r.ok = false;
        r.message = std::move(message);
        return r;
    }
    Result& with(std::string key, Value v) {
        data.emplace_back(std::move(key), std::move(v));
        return *this;
    }
    Result& attach(Attachment a) {
        attachments.push_back(std::move(a));
        return *this;
    }
    const Value* field(const std::string& key) const {
        for (const auto& [k, v] : data) {
            if (k == key) return &v;
        }
        return nullptr;
    }
};

// One concrete invocation of a command. Undoable actions must restore the
// exact prior state in revert().
class Action {
public:
    virtual ~Action() = default;
    virtual Result apply() = 0;
    virtual void revert() {}
};

// Wraps a plain function as a non-undoable action.
class FunctionAction : public Action {
public:
    explicit FunctionAction(std::function<Result()> fn) : fn_(std::move(fn)) {}
    Result apply() override { return fn_(); }

private:
    std::function<Result()> fn_;
};

using ActionFactory = std::function<std::unique_ptr<Action>(const Params&)>;

class ParamError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class CommandRegistry {
public:
    void add(CommandInfo info, ActionFactory factory);
    const CommandInfo* find(const std::string& id) const;
    std::vector<CommandInfo> list() const;

    // Validates params against the command's spec, then builds the action.
    // Throws ParamError on unknown commands, missing or mistyped params.
    std::unique_ptr<Action> create(const std::string& id, const Params& params) const;

private:
    struct Entry {
        CommandInfo info;
        ActionFactory factory;
    };
    std::map<std::string, Entry> entries_;
};

// Linear undo history. Each executed undoable action is one labeled step.
class History {
public:
    explicit History(std::size_t limit = 200) : limit_(limit) {}

    Result execute(std::unique_ptr<Action> action, const std::string& label, bool undoable);
    bool undo();
    bool redo();

    bool canUndo() const { return !done_.empty(); }
    bool canRedo() const { return !undone_.empty(); }
    std::vector<std::string> labels() const;  // oldest first

private:
    struct Step {
        std::string label;
        std::unique_ptr<Action> action;
    };
    std::size_t limit_;
    std::vector<Step> done_;
    std::vector<Step> undone_;
};

// Registry + history + open documents: what a tool, the CLI and the MCP server run commands through.
class Session {
public:
    CommandRegistry& registry() { return registry_; }
    const CommandRegistry& registry() const { return registry_; }
    History& history() { return history_; }
    Workspace& workspace() { return workspace_; }

    Result run(const std::string& id, const Params& params);

private:
    CommandRegistry registry_;
    History history_;
    Workspace workspace_;
};

// Param helpers for action factories (params are already validated).
std::string stringParam(const Params& p, const std::string& key, const std::string& fallback = {});
std::int64_t intParam(const Params& p, const std::string& key, std::int64_t fallback = 0);
double numberParam(const Params& p, const std::string& key, double fallback = 0.0);
bool boolParam(const Params& p, const std::string& key, bool fallback = false);
NumberList listParam(const Params& p, const std::string& key);

// Human-readable form of a value, as the CLI prints it.
std::string toText(const Value& value);

}  // namespace wetwaresuite
