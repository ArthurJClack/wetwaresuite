// The command system every tool is built on. Every user action is a named
// command with typed parameters, so the same action can come from a button,
// the `suite` CLI, a Python script or an MCP client.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace achclwss {

using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string>;
using Params = std::map<std::string, Value>;

enum class ParamType { Bool, Int, Number, String };

const char* toString(ParamType type);

struct ParamSpec {
    std::string name;
    ParamType type = ParamType::String;
    std::string description;
    bool required = true;
};

struct CommandInfo {
    std::string id;           // dotted, e.g. "toolkit.inspect"
    std::string title;        // shown in menus and as the undo label
    std::string description;  // shown to people and to MCP clients
    std::vector<ParamSpec> params;
    bool undoable = false;    // read-only and export commands are not
};

struct Result {
    bool ok = true;
    std::string message;
    Value value;

    static Result success(std::string message, Value value = {}) {
        return {true, std::move(message), std::move(value)};
    }
    static Result failure(std::string message) { return {false, std::move(message), {}}; }
};

// One concrete invocation of a command. Undoable actions must restore the
// exact prior state in revert().
class Action {
public:
    virtual ~Action() = default;
    virtual Result apply() = 0;
    virtual void revert() {}
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
    explicit History(std::size_t limit = 500) : limit_(limit) {}

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

// A registry plus a history: what a tool, the CLI and the MCP server run commands through.
class Session {
public:
    CommandRegistry& registry() { return registry_; }
    const CommandRegistry& registry() const { return registry_; }
    History& history() { return history_; }

    Result run(const std::string& id, const Params& params);

private:
    CommandRegistry registry_;
    History history_;
};

}  // namespace achclwss
