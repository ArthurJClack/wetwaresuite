// The office tool: spreadsheets (office.sheet_*), documents (office.doc_*),
// slide decks (office.deck_*), PDF tools (office.pdf_*) and office.convert,
// all headless commands with their own file formats (XLSX, CSV, DOCX,
// Markdown, HTML, PPTX, PDF). LibreOffice is an optional backend for the
// formats this tool does not write itself (ODT/ODS/ODP, DOC/XLS/PPT, RTF).
#pragma once

#include "achclwss/core/command.hpp"
#include "achclwss/office/chart.hpp"
#include "achclwss/office/deck.hpp"
#include "achclwss/office/doc.hpp"
#include "achclwss/office/sheet.hpp"

namespace achclwss::office {

void registerCommands(Session& session);

}  // namespace achclwss::office
