// The window's side of handing work between apps: what goes in the "Send to..."
// menu, the inbox banner on the home screen and the "Open in <app>" suggestion.
// The window learns all of it by running the app.* commands (app.installed,
// app.targets, app.inbox, app.open_with) through Session::run; these functions
// only turn those results into what to show, so ctest covers them without a screen.
#pragma once

#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::gui {

// "a,b,,c" -> {"a", "b", "c"}
std::vector<std::string> splitList(const std::string& csv);

// A result's data field as text ("" when missing).
std::string fieldText(const Result& result, const std::string& name);

// Display name of an app: "image" -> "Image", "cad" -> "CAD".
std::string appTitle(const std::string& appId);

// True when the session has the app.* handoff commands (every app binary does).
bool hasHandoff(const Session& session);

struct SendChoice {
    std::string app;         // "model"
    std::string label;       // "Model"
    bool installed = false;  // false: shown greyed out with "not installed"
    bool program = false;    // its own program is installed, so it can be started from here
};

// The "Send to..." menu from app.targets' result for the document on screen:
// one entry per app that takes it, installed ones first, never `self`.
std::vector<SendChoice> sendChoices(const Result& targets, const std::string& self);

struct InboxBanner {
    bool show = false;
    std::string text;    // "A texture from Image is waiting: brick.png"
    std::string button;  // "Open it"
};

// The home screen banner from app.inbox's result.
InboxBanner inboxBanner(const Result& inbox);

struct OpenSuggestion {
    bool show = false;
    std::string app;   // "model"
    std::string path;  // the file
    bool installed = false;
    std::string text;    // "brick.obj is a Model file."
    std::string button;  // "Open in Model"
};

// After opening (or failing to open) `path` here: from app.open_with's result,
// suggest the app the file belongs to when that's another app. `openedHere` is
// whether this window managed to open it itself.
OpenSuggestion openSuggestion(const Result& openWith, const std::string& self, const std::string& path, bool openedHere);

// After a successful app.send: the app to offer to start ("" = none: it opened
// here, or the target has no program of its own to start).
std::string launchAfterSend(const Result& sent, const std::string& self, const std::vector<SendChoice>& choices);

}  // namespace wetwaresuite::gui
