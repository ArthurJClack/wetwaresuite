// Helpers shared by the office.doc_* command sources (office-internal).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "commands.hpp"

namespace wetwaresuite::office::doccmd {

using Edit = std::function<Result(TextDoc&, const Params&, const std::string&)>;

ActionFactory edit(Session& s, Edit fn);  // undoable (snapshot)
ActionFactory read(Session& s, Edit fn);  // not undoable; file= works too

ParamSpec doc();
std::string describe(const Block& b, std::size_t index);
// block=n (1-based) to an index; false with a reason.
bool blockArg(const TextDocument& d, const Params& p, const char* key, std::size_t& index, std::string& error);
// Text with hard line breaks: each line is inline Markdown.
Runs inlineLines(const std::string& text);

// The run lists a command works on: a text block's runs, list items (item=n or all), table cells
// (row=/col= or all). False with a reason when the block has no text or the item/cell is missing.
std::vector<ParamSpec> targetSpecs();
bool targets(Block& b, const Params& p, std::vector<Runs*>& out, std::string& error);

// Makes byte offset `at` of the runs' plain text start a run; returns that run's index.
std::size_t splitRunsAt(Runs& runs, std::size_t at);
// Every match of `find` (all text when empty) as [start, end) byte ranges of the plain text.
std::vector<std::pair<std::size_t, std::size_t>> matches(const Runs& runs, const std::string& find, bool matchCase, bool wholeWord);
// Applies fn to the runs covering [a, b), splitting runs at the ends.
void applyRange(Runs& runs, std::size_t a, std::size_t b, const std::function<void(Run&)>& fn);

// Tracked changes: who edits, a new change, marking runs and a block.
std::string editorName(const TextDocument& d);
int newChange(TextDocument& d, bool insert);
// Deleting text that is itself a pending insertion removes it outright.
void markRuns(TextDocument& d, Runs& runs, int change);
bool markBlock(TextDocument& d, Block& b, int change);  // false when nothing is left to mark (images, page breaks)
// old -> new text as a deletion of what changed and an insertion of what replaces it.
Runs trackedReplace(const TextDocument& d, const Runs& oldRuns, const Runs& newRuns, int del, int ins);

void registerDocFormatCommands(Session& session);
void registerDocTableCommands(Session& session);
void registerDocReviewCommands(Session& session);

}  // namespace wetwaresuite::office::doccmd
