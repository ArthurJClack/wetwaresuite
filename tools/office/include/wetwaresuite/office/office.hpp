// The office tool: spreadsheets (office.sheet_*), documents (office.doc_*),
// slide decks (office.deck_*), PDF tools (office.pdf_*) and office.convert,
// all headless commands with their own file formats (XLSX, CSV, DOCX,
// Markdown, HTML, PPTX, PDF). LibreOffice is an optional backend for the
// formats this tool does not write itself (ODT/ODS/ODP, DOC/XLS/PPT, RTF).
#pragma once

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/office/chart.hpp"
#include "wetwaresuite/office/deck.hpp"
#include "wetwaresuite/office/doc.hpp"
#include "wetwaresuite/office/sheet.hpp"

namespace wetwaresuite::office {

void registerCommands(Session& session);

}  // namespace wetwaresuite::office
