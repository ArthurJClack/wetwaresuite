// Small helpers the cad command files share (internal).
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "wetwaresuite/cad/cad.hpp"
#include "wetwaresuite/mesh/document.hpp"

namespace wetwaresuite::cad::cmd {

inline std::string num(double v) {
    if (std::abs(v) < 5e-7) return "0";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

inline bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

inline ParamSpec docParam() { return {"doc", ParamType::String, "id of an open cad document, e.g. cad1", true}; }
inline ParamSpec str(const std::string& name, const std::string& help, bool required = false, std::string fallback = {}) {
    ParamSpec s{name, ParamType::String, help, required, {}, {}};
    if (!fallback.empty()) s.defaultValue = fallback;
    return s;
}
inline ParamSpec boolean(const std::string& name, const std::string& help, bool fallback) {
    return {name, ParamType::Bool, help, false, {}, fallback};
}

// The text of a param that may be a number or an expression.
inline std::string text(const Params& p, const std::string& key) {
    auto it = p.find(key);
    if (it == p.end()) return {};
    if (auto b = std::get_if<bool>(&it->second)) return *b ? "true" : "false";
    return toText(it->second);
}

using DocEdit = std::function<Result(CadDoc&, const Params&)>;

// Runs edit on the document named by "doc": undoable through CadEdit, or plainly.
inline ActionFactory cadCommand(Session& session, DocEdit edit, bool undoable = true) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<CadDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>(
                [id] { return Result::failure("no open cad document called '" + id + "' (use cad.new or cad.open first)"); });
        }
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, edit, p] {
                try {
                    return edit(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<CadEdit>(doc, [edit, p](CadDoc& d) { return edit(d, p); });
    };
}

}  // namespace wetwaresuite::cad::cmd
