// The window's layout per app (menus, ribbon tabs, save and close commands) from
// made-up commands: no window needed.
#include <algorithm>

#include "check.hpp"
#include "wetwaresuite/gui/logic.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::gui;

namespace {

CommandInfo cmd(std::string id, std::string title) { return {std::move(id), std::move(title), "Does a thing.", {}, true, false}; }

std::vector<CommandInfo> office() {
    return {cmd("office.sheet_new", "New Spreadsheet"), cmd("office.sheet_open", "Open Spreadsheet"),
            cmd("office.sheet_save", "Save Spreadsheet"), cmd("office.sheet_close", "Close Spreadsheet"),
            cmd("office.sheet_set", "Set Cells"),        cmd("office.sheet_sort", "Sort"),
            cmd("office.doc_new", "New Document"),        cmd("office.doc_save", "Save Document"),
            cmd("office.doc_add", "Add Block"),          cmd("office.convert", "Convert Office File"),
            cmd("pdf.open", "Open PDF"),                 cmd("pdf.save", "Save PDF"),
            cmd("pdf.close", "Close PDF"),               cmd("pdf.highlight", "Highlight"),
            cmd("notes.search", "Search Notes"),         cmd("notes.backlinks", "Backlinks")};
}

void testFileCommands() {
    CHECK(fileCommand("photo.open") && fileCommand("office.sheet_save") && fileCommand("cad.close"));
    CHECK(fileCommand("model.export") && fileCommand("cut.export_frame") && fileCommand("forge.sprite_export"));
    CHECK(!fileCommand("photo.resize") && !fileCommand("office.sheet_set") && !fileCommand("cad.extrude"));
}

void testMenuGroups() {
    std::vector<CommandInfo> cmds = {cmd("paint.layer_add", "New layer"),   cmd("paint.layer_delete", "Delete layer"),
                                     cmd("paint.layers", "Layers"),         cmd("paint.stroke", "Brush stroke"),
                                     cmd("paint.text", "Text layer"),       cmd("paint.text_edit", "Edit text"),
                                     cmd("paint.new", "New canvas"),        cmd("photo.resize", "Resize")};
    auto groups = menuGroups("paint", cmds);
    // flat first (stroke, the two text ones: only 2 share "text"), then the Layer submenu with the bare "layers"
    CHECK(groups.size() == 2);
    CHECK(groups[0].title.empty() && groups[0].commands.size() == 3);
    CHECK(groups[1].title == "Layer" && groups[1].commands.size() == 3);
    // File commands and other tools stay out
    for (const auto& g : groups) {
        for (const auto* c : g.commands) CHECK(c->id != "paint.new" && c->id != "photo.resize");
    }
    CHECK(groupTitle("doc") == "Document" && groupTitle("deck") == "Slides" && groupTitle("track") == "Track");
}

void testRibbon() {
    const auto cmds = office();
    const auto tabs = ribbonTabs("office", {"office", "pdf", "notes"}, cmds);
    // curated tabs whose commands exist: Document, Sheet, PDF; Notes falls back to its common actions; Slides has none here
    std::vector<std::string> titles;
    for (const auto& t : tabs) titles.push_back(t.title);
    CHECK((titles == std::vector<std::string>{"Document", "Sheet", "PDF", "Notes"}));
    const RibbonTab& sheet = tabs[1];
    CHECK(sheet.tool == "office" && sheet.stem == "sheet" && !sheet.groups.empty());
    CHECK(sheet.groups[0].title == "Cells" && sheet.groups[0].commands[0]->id == "office.sheet_set");
    CHECK(sheet.groups[0].labels[0] == "Set");  // "Set Cells" in the Cells group
    CHECK(sheet.groups[1].labels[0] == "Sort");
    for (const auto& t : tabs) {
        for (const auto& g : t.groups) {
            CHECK(g.labels.size() == g.commands.size());
            for (const auto* c : g.commands) CHECK(!fileCommand(c->id));
        }
    }
    // an app with no curated tabs (the all-in-one window): one tab per tool
    const auto all = ribbonTabs("suite", {"office", "pdf"}, cmds);
    CHECK(all.size() == 2 && all[0].title == "Office" && all[1].title == "PDF editor");
    // the tab for a file follows what opened it
    CHECK(ribbonTabFor(tabs, "office.sheet_open") == 1);
    CHECK(ribbonTabFor(tabs, "office.doc_new") == 0);
    CHECK(ribbonTabFor(tabs, "pdf.open") == 2);
    CHECK(ribbonTabFor(tabs, "cad.open") == -1 && ribbonTabFor(tabs, "") == -1);
    CHECK(appLayout("office").ribbon && !appLayout("office").compactRibbon);
    CHECK(appLayout("image").compactRibbon && !appLayout("system").ribbon);
}

void testSiblings() {
    const auto cmds = office();
    CHECK(openerStem("office.sheet_open") == "sheet" && openerStem("photo.open").empty() && openerStem("office.convert").empty());
    CHECK(siblingCommand("office.sheet_open", "save", cmds) == "office.sheet_save");
    CHECK(siblingCommand("office.sheet_new", "close", cmds) == "office.sheet_close");
    CHECK(siblingCommand("pdf.open", "save", cmds) == "pdf.save");
    CHECK(siblingCommand("office.doc_new", "close", cmds).empty());  // no office.doc_close and no office.close here
    CHECK(siblingCommand("", "save", cmds).empty());
}

void testShortLabels() {
    CHECK(shortLabel("Resize image", "Image", "Photo") == "Resize");
    CHECK(shortLabel("Set Cells", "Cells", "Sheet") == "Set");
    CHECK(shortLabel("Sketch", "Sketch", "Part") == "Sketch");  // nothing would be left
    CHECK(shortLabel("Crop to selection", "Image", "Photo") == "Crop to selection");
    CHECK(shortLabel("Add Layer", "Layers", "Level") == "Add");
}

}  // namespace

int main() {
    testFileCommands();
    testMenuGroups();
    testRibbon();
    testSiblings();
    testShortLabels();
    return check::summary("gui_layout");
}
