// The file manager panel: places, a sortable list with multi-select, copy,
// cut and paste, rename, new files and folders, zip and unzip, a preview, the
// trash with restore, and in Advanced mode find, checksums and folder sizes.
// Deleting moves things to the suite's own trash, and undo takes changes back.
#include <algorithm>
#include <cctype>
#include <ctime>

#include "tool_panels.hpp"
// after imgui.h
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

std::string when(const std::string& unixText) {
    std::time_t t = 0;
    try {
        t = static_cast<std::time_t>(std::stoll(unixText));
    } catch (...) {
        return {};
    }
    if (t <= 0) return {};
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

bool isArchive(const std::string& path) {
    const std::string p = tp::lowerAscii(path);
    for (const char* e : {".zip", ".tar", ".tgz", ".tar.gz", ".gz"}) {
        const std::string ext = e;
        if (p.size() > ext.size() && p.compare(p.size() - ext.size(), ext.size(), ext) == 0) return true;
    }
    return false;
}

std::string extensionOf(const std::string& path) {
    const std::string name = tp::baseName(path);
    const auto dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? std::string() : tp::lowerAscii(name.substr(dot + 1));
}

bool mentions(const std::string& text, const std::string& word) {
    if (word.empty()) return false;
    const std::string t = tp::lowerAscii(text);
    for (std::size_t at = t.find(word); at != std::string::npos; at = t.find(word, at + 1)) {
        const bool left = at == 0 || !std::isalnum(static_cast<unsigned char>(t[at - 1]));
        const std::size_t end = at + word.size();
        const bool right = end >= t.size() || !std::isalnum(static_cast<unsigned char>(t[end]));
        if (left && right) return true;
    }
    return false;
}

std::string joinLines(const std::vector<std::string>& paths) {
    std::string out;
    for (const auto& p : paths) out += (out.empty() ? "" : "\n") + p;
    return out;
}

}  // namespace

void Shell::explorerGo(const std::string& folder, bool remember) {
    ExplorerPanel& e = panels_->explorer;
    if (folder.empty()) return;
    if (remember && !e.folder.empty() && e.folder != folder) {
        e.back.push_back(e.folder);
        e.forward.clear();
    }
    e.folder = folder;
    e.pathInput = folder;
    e.selected.clear();
    e.stale = true;
    e.archive.clear();
    e.trashView = false;
    e.searchHits.clear();
    e.searching = false;
    e.previewFor.clear();
}

void Shell::explorerRefresh() {
    ExplorerPanel& e = panels_->explorer;
    if (e.places.empty()) e.places = tp::rows(query("explorer.places", {}));
    if (e.folder.empty()) explorerGo(e.places.empty() ? std::string("~") : e.places[0][1], false);
    if (!e.stale && e.seen == changes_) return;
    Result r = query("explorer.list", {{"folder", e.folder}, {"hidden", e.hidden}, {"sort", e.sort}, {"descending", e.descending}});
    e.stale = false;
    e.seen = changes_;
    if (!r.ok) {
        e.error = friendlyError(r.message);
        e.entries.clear();
        return;
    }
    e.error.clear();
    e.folder = tp::text(r, "folder");
    if (!ImGui::IsAnyItemActive()) e.pathInput = e.folder;
    e.entries = tp::rows(r);
    // forget selected names that are gone
    std::set<std::string> still;
    for (const auto& row : e.entries) {
        if (row.size() > 5 && e.selected.count(row[5])) still.insert(row[5]);
    }
    e.selected = still;
    if (e.trashView) e.trash = tp::rows(query("explorer.trash", {}));
}

void Shell::explorerOpen(const std::string& path, bool folder) {
    ExplorerPanel& e = panels_->explorer;
    if (folder) {
        explorerGo(path);
        return;
    }
    if (isArchive(path)) {
        Result r = query("explorer.archive_list", {{"path", path}});
        if (r.ok) {
            e.archive = path;
            e.archiveRows = tp::rows(r);
            e.archiveText = r.message.substr(r.message.rfind('\n') + 1);
            return;
        }
    }
    // a tool in this app that names the file type opens it here
    const std::string ext = extensionOf(path);
    for (const auto& tool : tools_) {
        if (tool == "explorer" || tool == "terminal" || tool == "code") continue;
        const CommandInfo* c = find(tool + ".open");
        if (c && mentions(c->description + " " + c->title, ext)) {
            openFile(path);
            return;
        }
    }
    if (hasPanel("code")) {
        Result p = query("explorer.preview", {{"path", path}, {"lines", std::int64_t{1}}});
        if (tp::text(p, "kind") == "text") {
            codeOpenAt(path, 0);
            return;
        }
    }
    Result r = run("explorer.open", {{"path", path}});
    e.status = r.ok ? "Opened " + tp::baseName(path) + " in its app." : friendlyError(r.message);
}

void Shell::explorerToolbar() {
    ExplorerPanel& e = panels_->explorer;
    const float unit = ImGui::GetFontSize();
    ImGui::BeginDisabled(e.back.empty());
    if (ImGui::Button("<")) {
        e.forward.push_back(e.folder);
        const std::string to = e.back.back();
        e.back.pop_back();
        explorerGo(to, false);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Back");
    ImGui::SameLine();
    ImGui::BeginDisabled(e.forward.empty());
    if (ImGui::Button(">")) {
        e.back.push_back(e.folder);
        const std::string to = e.forward.back();
        e.forward.pop_back();
        explorerGo(to, false);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Forward");
    ImGui::SameLine();
    if (ImGui::Button("Up")) explorerGo(tp::parentOf(e.folder));
    ImGui::SetItemTooltip("The folder this one is in (Backspace)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(unit * 8, ImGui::GetContentRegionAvail().x - unit * 13));
    if (ImGui::InputText("##path", &e.pathInput, ImGuiInputTextFlags_EnterReturnsTrue)) explorerGo(e.pathInput);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter names", &e.filter);

    // actions on the selection
    std::vector<std::string> sel(e.selected.begin(), e.selected.end());
    const bool one = sel.size() == 1, any = !sel.empty();
    if (ImGui::Button("New folder")) {
        e.nameInput = "New folder";
        ImGui::OpenPopup("newfolder");
    }
    tp::flow("New file");
    if (ImGui::Button("New file")) {
        e.nameInput = "notes.txt";
        ImGui::OpenPopup("newfile");
    }
    tp::flow("Copy");
    ImGui::BeginDisabled(!any);
    if (ImGui::Button("Copy")) {
        e.clip = sel;
        e.cut = false;
        e.status = std::to_string(sel.size()) + " item(s) ready to paste.";
    }
    tp::flow("Cut");
    if (ImGui::Button("Cut")) {
        e.clip = sel;
        e.cut = true;
        e.status = std::to_string(sel.size()) + " item(s) will move when you paste.";
    }
    ImGui::EndDisabled();
    tp::flow("Paste");
    ImGui::BeginDisabled(e.clip.empty());
    if (ImGui::Button("Paste")) {
        Result r = run(e.cut ? "explorer.move" : "explorer.copy", {{"paths", joinLines(e.clip)}, {"to", e.folder}});
        e.status = r.ok ? sentence(r.message) : friendlyError(r.message);
        if (r.ok && e.cut) e.clip.clear();
    }
    ImGui::EndDisabled();
    tp::flow("Rename");
    ImGui::BeginDisabled(!one);
    if (ImGui::Button("Rename")) {
        e.renaming = sel[0];
        e.nameInput = tp::baseName(sel[0]);
        ImGui::OpenPopup("rename");
    }
    ImGui::EndDisabled();
    tp::flow("Delete");
    ImGui::BeginDisabled(!any);
    if (ImGui::Button("Delete")) {
        Result r = run("explorer.delete", {{"paths", joinLines(sel)}});
        e.status = r.ok ? sentence(r.message) + " Undo brings it back." : friendlyError(r.message);
    }
    ImGui::SetItemTooltip("Move to the trash (Delete key). Undo or the trash bring it back.");
    tp::flow("Zip");
    if (ImGui::Button("Zip")) {
        e.nameInput = (one ? tp::baseName(sel[0]) : std::string("archive")) + ".zip";
        ImGui::OpenPopup("zip");
    }
    ImGui::SetItemTooltip("Pack the selected files into a zip file");
    ImGui::EndDisabled();
    if (one && isArchive(sel[0])) {
        tp::flow("Extract");
        if (ImGui::Button("Extract")) {
            Result r = run("explorer.extract", {{"path", sel[0]}});
            e.status = r.ok ? sentence(r.message) : friendlyError(r.message);
        }
    }
    if (hasPanel("terminal")) {
        tp::flow("Terminal here");
        if (ImGui::Button("Terminal here")) folderChosen("terminal", e.folder);
    }
    if (hasPanel("code")) {
        tp::flow("Open as project");
        if (ImGui::Button("Open as project")) folderChosen("code", e.folder);
        ImGui::SetItemTooltip("Open this folder in the code editor");
    }
    if (pro_) {
        ImGui::SameLine();
        ImGui::Checkbox("Hidden", &e.hidden);
        if (ImGui::IsItemEdited()) e.stale = true;
        tp::flow("Find...");
        if (ImGui::Button("Find...")) e.searching = !e.searching;
        tp::flow("Checksum");
        ImGui::BeginDisabled(!any);
        if (ImGui::Button("Checksum")) {
            Result r = query("explorer.checksum", {{"paths", joinLines(sel)}});
            e.previewFor = "#info";
            e.previewKind = "info";
            e.previewText = r.ok ? r.message : friendlyError(r.message);
        }
        ImGui::EndDisabled();
        tp::flow("Folder size");
        if (ImGui::Button("Folder size")) {
            Result r = query("explorer.size", {{"folder", e.folder}});
            e.previewFor = "#info";
            e.previewKind = "info";
            e.previewText = r.ok ? r.message : friendlyError(r.message);
        }
    }
    explorerPopups();
}

void Shell::explorerPopups() {
    ExplorerPanel& e = panels_->explorer;
    auto namePopup = [&](const char* id, const char* prompt, const char* button, auto&& act) {
        if (!ImGui::BeginPopup(id)) return;
        ImGui::TextUnformatted(prompt);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
        const bool go = ImGui::InputText("##name", &e.nameInput, ImGuiInputTextFlags_EnterReturnsTrue);
        if ((accentButton(button) || go) && !e.nameInput.empty()) {
            Result r = act();
            e.status = r.ok ? sentence(r.message) : friendlyError(r.message);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    };
    namePopup("newfolder", "Name of the new folder", "Make folder",
              [&] { return run("explorer.new_folder", {{"path", tp::joinPath(e.folder, e.nameInput)}}); });
    namePopup("newfile", "Name of the new file", "Make file", [&] { return run("explorer.new_file", {{"path", tp::joinPath(e.folder, e.nameInput)}}); });
    namePopup("rename", "New name", "Rename", [&] { return run("explorer.rename", {{"path", e.renaming}, {"name", e.nameInput}}); });
    namePopup("zip", "Name of the zip file", "Make zip", [&] {
        std::vector<std::string> sel(e.selected.begin(), e.selected.end());
        return run("explorer.archive", {{"paths", joinLines(sel)}, {"output", tp::joinPath(e.folder, e.nameInput)}});
    });
}

void Shell::explorerSide() {
    ExplorerPanel& e = panels_->explorer;
    tp::title("Places", 1.05f);
    for (std::size_t i = 0; i < e.places.size(); ++i) {
        const auto& p = e.places[i];
        if (p.size() < 2) continue;
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(p[0].c_str(), !e.trashView && e.folder == p[1])) explorerGo(p[1]);
        ImGui::SetItemTooltip("%s", p[1].c_str());
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (ImGui::Selectable("Trash", e.trashView)) {
        e.trashView = true;
        e.archive.clear();
        e.trash = tp::rows(query("explorer.trash", {}));
    }
}

void Shell::explorerTrash() {
    ExplorerPanel& e = panels_->explorer;
    tp::title("Trash", 1.2f);
    tp::muted("Things you deleted here wait in the suite's trash until you empty it.");
    if (ImGui::Button("Back to files")) e.trashView = false;
    ImGui::SameLine();
    ImGui::BeginDisabled(e.trash.empty());
    if (ImGui::Button("Empty trash")) ImGui::OpenPopup("emptytrash");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("emptytrash")) {
        ImGui::TextUnformatted("Delete everything in the trash for good? This can't be undone.");
        if (accentButton("Empty it")) {
            Result r = run("explorer.empty_trash", {});
            e.status = r.ok ? sentence(r.message) : friendlyError(r.message);
            e.trash = tp::rows(query("explorer.trash", {}));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep it")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (e.trash.empty()) tp::muted("The trash is empty.");
    if (!e.trash.empty() && ImGui::BeginTable("trash", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Was in");
        ImGui::TableSetupColumn("Deleted");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < e.trash.size(); ++i) {
            const auto& t = e.trash[i];
            if (t.size() < 4) continue;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(tp::baseName(t[1]).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", tp::parentOf(t[1]).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(when(t[2]).c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Restore")) {
                Result r = run("explorer.restore", {{"id", t[0]}});
                e.status = r.ok ? sentence(r.message) : friendlyError(r.message);
                e.trash = tp::rows(query("explorer.trash", {}));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void Shell::explorerArchive() {
    ExplorerPanel& e = panels_->explorer;
    tp::title(tp::baseName(e.archive), 1.2f);
    tp::muted(e.archiveText);
    if (ImGui::Button("Back to files")) e.archive.clear();
    ImGui::SameLine();
    if (accentButton("Extract here")) {
        Result r = run("explorer.extract", {{"path", e.archive}});
        e.status = r.ok ? sentence(r.message) : friendlyError(r.message);
        if (r.ok) e.archive.clear();
        return;
    }
    ImGui::SetItemTooltip("Unpack into a new folder next to the archive. Undo takes it away again.");
    if (ImGui::BeginTable("archive", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Modified");
        ImGui::TableHeadersRow();
        for (const auto& a : e.archiveRows) {
            if (a.size() < 5) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a[1].c_str());
            ImGui::TableNextColumn();
            if (a[0] != "dir") ImGui::TextUnformatted(tp::humanSize(std::stod(a[2])).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(when(a[4]).c_str());
        }
        ImGui::EndTable();
    }
}

void Shell::explorerList() {
    ExplorerPanel& e = panels_->explorer;
    if (e.searching) {
        const float unit = ImGui::GetFontSize();
        ImGui::SetNextItemWidth(unit * 10);
        bool go = ImGui::InputTextWithHint("##sname", "name, e.g. *.pdf", &e.searchName, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(unit * 10);
        go = ImGui::InputTextWithHint("##scont", "text inside", &e.searchContains, ImGuiInputTextFlags_EnterReturnsTrue) || go;
        ImGui::SameLine();
        if (accentButton("Find in subfolders") || go) {
            Params p{{"folder", e.folder}, {"limit", std::int64_t{500}}};
            if (!e.searchName.empty()) p["name"] = e.searchName;
            if (!e.searchContains.empty()) p["contains"] = e.searchContains;
            Result r = query("explorer.search", p);
            e.searchHits = tp::rows(r);
            e.status = r.ok ? r.message.substr(0, r.message.find('\n')) : friendlyError(r.message);
        }
    }
    const tp::Rows& list = e.searching && !e.searchHits.empty() ? e.searchHits : e.entries;
    std::vector<const std::vector<std::string>*> shownRows;
    const std::string filter = tp::lowerAscii(e.filter);
    for (const auto& row : list) {
        if (row.size() < 6) continue;
        if (!filter.empty() && tp::lowerAscii(row[1]).find(filter) == std::string::npos) continue;
        shownRows.push_back(&row);
    }
    tp::errorText(e.error);
    const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_BordersInnerV;
    const float unit = ImGui::GetFontSize();
    const float width = ImGui::GetContentRegionAvail().x;
    const int columns = width > unit * 30 ? 4 : width > unit * 21 ? 3 : 2;  // narrow: drop Type, then Modified
    if (!ImGui::BeginTable("files", columns, flags, {0, -ImGui::GetFrameHeightWithSpacing()})) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, unit * 4.2f);
    if (columns > 2) ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, unit * 7.0f);
    if (columns > 3) ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, unit * 5.0f);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty && specs->SpecsCount > 0) {
        static const char* sorts[] = {"name", "size", "date", "type"};  // columns in this order
        e.sort = sorts[std::clamp(static_cast<int>(specs->Specs[0].ColumnIndex), 0, 3)];
        e.descending = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
        e.stale = true;
        specs->SpecsDirty = false;
    }
    std::string open;
    bool openFolderRow = false;
    for (std::size_t i = 0; i < shownRows.size(); ++i) {
        const auto& row = *shownRows[i];
        const std::string& path = row[5];
        const bool dir = row[0] == "dir";
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(path.c_str());
        const bool isSel = e.selected.count(path) > 0;
        const std::string label = dir ? row[1] + "/" : row[1];
        if (ImGui::Selectable(label.c_str(), isSel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyShift && !e.lastClicked.empty()) {
                // a range from the last clicked row
                bool in = false;
                for (const auto* r : shownRows) {
                    const bool edge = (*r)[5] == e.lastClicked || (*r)[5] == path;
                    if (edge || in) e.selected.insert((*r)[5]);
                    if (edge && e.lastClicked != path) in = !in;
                }
            } else if (io.KeyCtrl || io.KeySuper) {
                if (isSel) e.selected.erase(path);
                else e.selected.insert(path);
            } else {
                e.selected = {path};
            }
            e.lastClicked = path;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                open = path;
                openFolderRow = dir;
            }
        }
        if (ImGui::BeginPopupContextItem("ctx")) {
            if (!e.selected.count(path)) e.selected = {path};
            if (ImGui::MenuItem("Open")) {
                open = path;
                openFolderRow = dir;
            }
            if (!dir && ImGui::MenuItem("Open with its usual app")) run("explorer.open", {{"path", path}});
            if (ImGui::MenuItem("Copy")) {
                e.clip.assign(e.selected.begin(), e.selected.end());
                e.cut = false;
            }
            if (ImGui::MenuItem("Cut")) {
                e.clip.assign(e.selected.begin(), e.selected.end());
                e.cut = true;
            }
            if (ImGui::MenuItem("Move to trash")) {
                std::vector<std::string> sel(e.selected.begin(), e.selected.end());
                Result r = run("explorer.delete", {{"paths", joinLines(sel)}});
                e.status = r.ok ? sentence(r.message) + " Undo brings it back." : friendlyError(r.message);
            }
            if (isArchive(path) && ImGui::MenuItem("Extract here")) {
                Result r = run("explorer.extract", {{"path", path}});
                e.status = r.ok ? sentence(r.message) : friendlyError(r.message);
            }
            if (ImGui::MenuItem("Info")) {
                Result r = query("explorer.info", {{"path", path}});
                e.previewFor = "#info";
                e.previewKind = "info";
                e.previewText = r.ok ? r.message : friendlyError(r.message);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::TableNextColumn();
        if (!dir) ImGui::TextUnformatted(tp::humanSize(std::stod(row[2])).c_str());
        if (columns > 2) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(when(row[3]).c_str());
        }
        if (columns > 3) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", e.searching && !e.searchHits.empty() ? tp::parentOf(path).c_str() : row[4].c_str());
        }
    }
    ImGui::EndTable();
    std::size_t files = 0;
    for (const auto* r : shownRows) files += (*r)[0] != "dir";
    ImGui::TextDisabled("%zu item(s), %zu selected   %s", shownRows.size(), e.selected.size(), e.status.c_str());
    (void)files;
    if (!open.empty()) explorerOpen(open, openFolderRow);
}

void Shell::explorerPreview() {
    ExplorerPanel& e = panels_->explorer;
    if (e.previewFor != "#info") {
        const std::string want = e.selected.size() == 1 ? *e.selected.begin() : std::string();
        if (want != e.previewFor) {
            e.previewFor = want;
            e.previewKind.clear();
            e.previewText.clear();
            e.previewInfo.clear();
            e.previewImage.reset();
            if (!want.empty()) {
                Result r = query("explorer.preview", {{"path", want}, {"lines", std::int64_t{60}}, {"size", std::int64_t{384}}});
                e.previewKind = tp::text(r, "kind");
                const std::string& m = r.message;
                const auto first = m.find('\n');
                const auto second = first == std::string::npos ? std::string::npos : m.find('\n', first + 1);
                e.previewInfo = first == std::string::npos ? m : m.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
                if (e.previewKind == "image") tp::attachmentTexture(renderer_, e.previewImage, r);
                else if (e.previewKind == "text") e.previewText = tp::text(r, "text");
                else e.previewText = second == std::string::npos ? std::string() : m.substr(second + 1);
                if (!r.ok) e.previewText = friendlyError(r.message);
            }
        }
    }
    if (e.previewFor.empty()) {
        tp::muted("Pick a file to see a preview.");
        return;
    }
    if (e.previewFor == "#info") {
        if (ImGui::SmallButton("Close")) e.previewFor = "-";
    } else {
        tp::title(tp::baseName(e.previewFor), 1.1f);
        tp::muted(e.previewInfo);
    }
    ImGui::BeginChild("previewbody", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    if (e.previewKind == "image") {
        tp::fitImage(e.previewImage, ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
    } else {
        ImGui::PushFont(monoFont, ImGui::GetStyle().FontSizeBase * 0.8f);
        ImGui::TextUnformatted(e.previewText.c_str(), e.previewText.c_str() + e.previewText.size());
        ImGui::PopFont();
    }
    ImGui::EndChild();
}

void Shell::explorerPanel() {
    ExplorerPanel& e = panels_->explorer;
    explorerRefresh();
    // keys when nothing is being typed
    if (!ImGui::GetIO().WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
        std::vector<std::string> sel(e.selected.begin(), e.selected.end());
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !sel.empty()) {
            Result r = run("explorer.delete", {{"paths", joinLines(sel)}});
            e.status = r.ok ? sentence(r.message) + " Undo brings it back." : friendlyError(r.message);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) explorerGo(tp::parentOf(e.folder));
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C) && !sel.empty()) {
            e.clip = sel;
            e.cut = false;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X) && !sel.empty()) {
            e.clip = sel;
            e.cut = true;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V) && !e.clip.empty()) {
            Result r = run(e.cut ? "explorer.move" : "explorer.copy", {{"paths", joinLines(e.clip)}, {"to", e.folder}});
            e.status = r.ok ? sentence(r.message) : friendlyError(r.message);
            if (r.ok && e.cut) e.clip.clear();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) e.stale = true;
    }
    explorerToolbar();
    const float unit = ImGui::GetFontSize();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float sideW = std::clamp(avail * 0.17f, unit * 7, unit * 12);
    const bool showPreview = avail > unit * 34;
    const float previewW = showPreview ? std::clamp(avail * 0.26f, unit * 9, unit * 20) : 0.0f;
    ImGui::BeginChild("places", {sideW, 0}, ImGuiChildFlags_Borders);
    explorerSide();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("list", {showPreview ? std::max(unit * 10, ImGui::GetContentRegionAvail().x - previewW - ImGui::GetStyle().ItemSpacing.x) : 0.0f, 0},
                      ImGuiChildFlags_Borders);
    if (e.trashView) explorerTrash();
    else if (!e.archive.empty()) explorerArchive();
    else explorerList();
    ImGui::EndChild();
    if (!showPreview) return;
    ImGui::SameLine();
    ImGui::BeginChild("preview", {0, 0}, ImGuiChildFlags_Borders);
    explorerPreview();
    ImGui::EndChild();
}

}  // namespace wetwaresuite::gui
