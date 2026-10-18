#include "achclwss/core/command.hpp"

#include <sstream>

namespace achclwss {

const char* toString(ParamType type) {
    switch (type) {
        case ParamType::Bool: return "boolean";
        case ParamType::Int: return "integer";
        case ParamType::Number: return "number";
        case ParamType::String: return "string";
        case ParamType::NumberList: return "array";
    }
    return "string";
}

namespace {

bool matches(ParamType type, const Value& value) {
    switch (type) {
        case ParamType::Bool: return std::holds_alternative<bool>(value);
        case ParamType::Int: return std::holds_alternative<std::int64_t>(value);
        case ParamType::Number:
            return std::holds_alternative<double>(value) || std::holds_alternative<std::int64_t>(value);
        case ParamType::String: return std::holds_alternative<std::string>(value);
        case ParamType::NumberList: return std::holds_alternative<NumberList>(value);
    }
    return false;
}

}  // namespace

void CommandRegistry::add(CommandInfo info, ActionFactory factory) {
    auto id = info.id;
    entries_.insert_or_assign(id, Entry{std::move(info), std::move(factory)});
}

const CommandInfo* CommandRegistry::find(const std::string& id) const {
    auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second.info;
}

std::vector<CommandInfo> CommandRegistry::list() const {
    std::vector<CommandInfo> out;
    out.reserve(entries_.size());
    for (const auto& [id, entry] : entries_) out.push_back(entry.info);
    return out;
}

std::unique_ptr<Action> CommandRegistry::create(const std::string& id, const Params& params) const {
    auto it = entries_.find(id);
    if (it == entries_.end()) throw ParamError("unknown command: " + id);
    const auto& info = it->second.info;

    for (const auto& [name, value] : params) {
        bool known = false;
        for (const auto& spec : info.params) known = known || spec.name == name;
        if (!known) throw ParamError("unknown parameter '" + name + "' for " + id);
    }
    for (const auto& spec : info.params) {
        auto p = params.find(spec.name);
        if (p == params.end() || std::holds_alternative<std::monostate>(p->second)) {
            if (spec.required) throw ParamError("missing parameter '" + spec.name + "' for " + id);
            continue;
        }
        if (!matches(spec.type, p->second)) {
            throw ParamError("parameter '" + spec.name + "' for " + id + " must be " + toString(spec.type));
        }
        if (!spec.choices.empty()) {
            const auto& v = std::get<std::string>(p->second);
            bool allowed = false;
            for (const auto& c : spec.choices) allowed = allowed || c == v;
            if (!allowed) {
                std::string list;
                for (const auto& c : spec.choices) list += (list.empty() ? "" : ", ") + c;
                throw ParamError("parameter '" + spec.name + "' for " + id + " must be one of: " + list);
            }
        }
    }
    return it->second.factory(params);
}

Result History::execute(std::unique_ptr<Action> action, const std::string& label, bool undoable) {
    Result result = action->apply();
    if (!result.ok || !undoable) return result;
    undone_.clear();
    done_.push_back({label, std::move(action)});
    if (done_.size() > limit_) done_.erase(done_.begin());
    return result;
}

bool History::undo() {
    if (done_.empty()) return false;
    auto step = std::move(done_.back());
    done_.pop_back();
    step.action->revert();
    undone_.push_back(std::move(step));
    return true;
}

bool History::redo() {
    if (undone_.empty()) return false;
    auto step = std::move(undone_.back());
    undone_.pop_back();
    step.action->apply();
    done_.push_back(std::move(step));
    return true;
}

std::vector<std::string> History::labels() const {
    std::vector<std::string> out;
    for (const auto& step : done_) out.push_back(step.label);
    return out;
}

Result Session::run(const std::string& id, const Params& params) {
    std::unique_ptr<Action> action;
    try {
        action = registry_.create(id, params);
    } catch (const ParamError& e) {
        return Result::failure(e.what());
    }
    const auto* info = registry_.find(id);
    return history_.execute(std::move(action), info->title, info->undoable);
}

std::string stringParam(const Params& p, const std::string& key, const std::string& fallback) {
    auto it = p.find(key);
    if (it == p.end()) return fallback;
    if (auto v = std::get_if<std::string>(&it->second)) return *v;
    return fallback;
}

std::int64_t intParam(const Params& p, const std::string& key, std::int64_t fallback) {
    auto it = p.find(key);
    if (it == p.end()) return fallback;
    if (auto v = std::get_if<std::int64_t>(&it->second)) return *v;
    return fallback;
}

double numberParam(const Params& p, const std::string& key, double fallback) {
    auto it = p.find(key);
    if (it == p.end()) return fallback;
    if (auto v = std::get_if<double>(&it->second)) return *v;
    if (auto v = std::get_if<std::int64_t>(&it->second)) return static_cast<double>(*v);
    return fallback;
}

bool boolParam(const Params& p, const std::string& key, bool fallback) {
    auto it = p.find(key);
    if (it == p.end()) return fallback;
    if (auto v = std::get_if<bool>(&it->second)) return *v;
    return fallback;
}

NumberList listParam(const Params& p, const std::string& key) {
    auto it = p.find(key);
    if (it == p.end()) return {};
    if (auto v = std::get_if<NumberList>(&it->second)) return *v;
    return {};
}

std::string toText(const Value& value) {
    if (auto s = std::get_if<std::string>(&value)) return *s;
    if (auto b = std::get_if<bool>(&value)) return *b ? "true" : "false";
    if (auto i = std::get_if<std::int64_t>(&value)) return std::to_string(*i);
    if (auto d = std::get_if<double>(&value)) {
        std::ostringstream out;
        out << *d;
        return out.str();
    }
    if (auto l = std::get_if<NumberList>(&value)) {
        std::string out;
        for (double d : *l) {
            if (!out.empty()) out += ",";
            out += toText(d);
        }
        return out;
    }
    return {};
}

}  // namespace achclwss
