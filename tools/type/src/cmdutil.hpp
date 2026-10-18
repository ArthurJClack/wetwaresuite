// Shared pieces of the type.* command definitions.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "internal.hpp"

namespace achclwss::typeface::cmd {

using Edit = std::function<Result(FontDoc&, const Params&)>;

bool has(const Params& p, const std::string& key);
std::string plural(std::size_t n, const std::string& word);
std::vector<std::string> splitList(const std::string& text, char sep = ',');

ParamSpec docParam();
ParamSpec previewParam();
// For commands that only read the font (info, export, render, proof, ...): doc= or font=<file>.
ParamSpec docOrFileParam();
ParamSpec fontFileParam();
ParamSpec glyphParam(const std::string& what = "glyph: its name (A, a, zero, uni00E9), a character (é) or U+00E9");
ParamSpec glyphsParam(const std::string& what = "glyphs to change: names or characters, comma-separated, ranges like A-Z or "
                                                 "U+0100-U+017F, the words uppercase, lowercase, digits, letters, ascii, latin1, "
                                                 "encoded, unencoded, or all (default all)");

// Glyph indexes for a glyphs= spec (empty = all). Fails with a message naming what is missing.
bool glyphList(const Font& font, const std::string& spec, std::vector<int>& out, std::string& error);
// Index of an existing glyph, or an error naming it.
int requireGlyph(const Font& font, const std::string& ref, std::string& error);
// Name and code point for a new glyph from name= / unicode= / a single character.
bool newGlyphIdentity(const Font& font, const std::string& nameOrChar, const std::string& unicode, std::string& name,
                      std::vector<std::uint32_t>& unicodes, std::string& error);
// Unicode list "A,U+00C1,0xE9" (or "none"). Fails on anything that isn't a code point.
bool parseUnicodeList(const std::string& text, std::vector<std::uint32_t>& out, std::string& error);

// The undoable (or plain) action for an edit of an open font, with preview= handling:
// the result's "glyph" field picks a glyph view, otherwise the overview is attached.
ActionFactory fontCommand(Session& session, Edit edit, bool undoable = true);
Result& maybePreview(Result& r, const FontDoc& doc, const Params& p);
Result created(Session& session, std::shared_ptr<FontDoc> doc, const std::string& verb, const Params& p,
               const std::vector<std::string>& warnings = {});
Attachment png(const Image& img, const std::string& name);
std::string glyphLine(const Font& font, const Glyph& g);

// Command groups.
void registerFileCommands(Session& session);
void registerGlyphCommands(Session& session);
void registerShapeCommands(Session& session);
void registerSpacingCommands(Session& session);
void registerOutputCommands(Session& session);
void registerTraceCommands(Session& session);

}  // namespace achclwss::typeface::cmd
