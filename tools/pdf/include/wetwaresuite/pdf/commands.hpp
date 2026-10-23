// The pdf tool: an Acrobat-style PDF editor. Open any PDF, view its pages,
// add text and pictures, edit and replace its text, highlight, comment, draw,
// redact, fill in forms, sign, and turn, move, insert or delete pages; pdf.save
// writes the result. Every command runs headless and over MCP.
#pragma once

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::pdf {

void registerCommands(Session& session);

}  // namespace wetwaresuite::pdf
