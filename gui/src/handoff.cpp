#include "wetwaresuite/gui/handoff.hpp"

#include <algorithm>
#include <cctype>

namespace wetwaresuite::gui {

namespace {

std::string fileName(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

std::string withArticle(const std::string& noun) {
    if (noun.empty()) return "A file";
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(noun[0])));
    const bool vowel = c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
    return (vowel ? "An " : "A ") + noun;
}

}  // namespace

std::vector<std::string> splitList(const std::string& csv) {
    std::vector<std::string> out;
    std::string item;
    for (char c : csv + ",") {
        if (c != ',') {
            item += c;
            continue;
        }
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (!item.empty()) out.push_back(item);
        item.clear();
    }
    return out;
}

std::string fieldText(const Result& result, const std::string& name) {
    const Value* v = result.field(name);
    return v ? toText(*v) : std::string();
}

std::string appTitle(const std::string& appId) {
    if (appId == "cad") return "CAD";
    if (appId == "suite" || appId == "all") return "wetwaresuite";
    std::string out = appId;
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

bool hasHandoff(const Session& session) {
    const auto& reg = session.registry();
    return reg.find("app.installed") && reg.find("app.send") && reg.find("app.inbox") && reg.find("app.receive");
}

std::vector<SendChoice> sendChoices(const Result& targets, const std::string& self) {
    std::vector<SendChoice> on, off;
    if (!targets.ok) return on;
    const auto installed = splitList(fieldText(targets, "installed"));
    const auto programs = splitList(fieldText(targets, "programs"));
    for (const auto& app : splitList(fieldText(targets, "targets"))) {
        if (app == self) continue;
        SendChoice c{app, appTitle(app), contains(installed, app), contains(programs, app)};
        (c.installed ? on : off).push_back(c);
    }
    on.insert(on.end(), off.begin(), off.end());
    return on;
}

InboxBanner inboxBanner(const Result& inbox) {
    InboxBanner b;
    if (!inbox.ok) return b;
    const Value* countValue = inbox.field("count");
    const auto* count = countValue ? std::get_if<std::int64_t>(countValue) : nullptr;
    if (!count || *count <= 0) return b;
    b.show = true;
    const std::string from = fieldText(inbox, "from");
    std::string kind = fieldText(inbox, "kind");
    if (kind == "audio" || kind == "music") kind += " file";  // "an audio file", not "an audio"
    std::string what = withArticle(kind.empty() ? "file" : kind);
    if (!from.empty()) what += " from " + appTitle(from);
    b.text = what + " is waiting: " + fileName(fieldText(inbox, "file"));
    if (*count > 1) b.text += " (and " + std::to_string(*count - 1) + " more)";
    b.text += ".";
    b.button = "Open it";
    return b;
}

OpenSuggestion openSuggestion(const Result& openWith, const std::string& self, const std::string& path, bool openedHere) {
    OpenSuggestion s;
    if (!openWith.ok || self == "suite" || self.empty()) return s;
    const std::string app = fieldText(openWith, "app");
    if (app.empty() || app == self || app == "files") return s;
    s.show = true;
    s.app = app;
    s.path = path;
    s.installed = fieldText(openWith, "program") == "true";
    const std::string name = fileName(path);
    s.text = openedHere ? name + " usually belongs in the " + appTitle(app) + " app."
                        : "This app can't open " + name + ": it's a file for the " + appTitle(app) + " app.";
    if (!s.installed) s.text += " " + appTitle(app) + " isn't installed yet: add it with the wetwaresuite hub.";
    s.button = "Open in " + appTitle(app);
    return s;
}

std::string launchAfterSend(const Result& sent, const std::string& self, const std::vector<SendChoice>& choices) {
    if (!sent.ok || fieldText(sent, "opened") == "true") return {};
    const std::string to = fieldText(sent, "to");
    if (to.empty() || to == self) return {};
    for (const auto& c : choices)
        if (c.app == to && c.program) return to;
    return {};
}

}  // namespace wetwaresuite::gui
