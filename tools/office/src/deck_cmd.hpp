// Helpers shared by the office.deck_* command sources (office-internal).
#pragma once

#include <functional>
#include <string>

#include "commands.hpp"

namespace wetwaresuite::office::deckcmd {

using Edit = std::function<Result(DeckDoc&, const Params&, const std::string&)>;

ActionFactory edit(Session& s, Edit fn);  // undoable (snapshot)
ActionFactory read(Session& s, Edit fn);  // not undoable; file= works too

ParamSpec doc();
// slide=n (1-based) to an index; false with a reason.
bool slideArg(const Deck& d, const Params& p, const char* key, std::size_t& index, std::string& error);
std::string describe(const Deck& d, std::size_t index);
// Attaches the slide as a PNG when preview=true.
void slidePreview(Result& r, const Deck& d, std::size_t index, const Params& p);

void registerDeckObjectCommands(Session& session);  // text boxes, shapes, lines, pictures
void registerDeckShowCommands(Session& session);    // transitions, master, notes, slide shows

}  // namespace wetwaresuite::office::deckcmd
