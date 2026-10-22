// Shell state and the actions every panel shares. Everything that changes a
// document goes through Session::run, so the window, scripts and MCP clients
// all see the same undo history.
#include <algorithm>
#include <cctype>
#include <mutex>
#include <fstream>
#include <set>
#include <sstream>

#include "shell.hpp"
#include "tool_panels.hpp"

namespace wetwaresuite::gui {

Uint32 Shell::wakeEvent = 0;

void Texture::reset() {
    if (tex) SDL_DestroyTexture(tex);
    tex = nullptr;
    width = height = srcWidth = srcHeight = 0;
}

bool Texture::upload(SDL_Renderer* renderer, const std::uint8_t* rgba, int w, int h) {
    srcWidth = w;
    srcHeight = h;
    // big pictures are shown at a lower resolution than the renderer's limit; edits still use full size
    int maxSide = static_cast<int>(SDL_GetNumberProperty(SDL_GetRendererProperties(renderer),
                                                         SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 4096));
    if (maxSide <= 0) maxSide = 4096;
    int step = 1;
    while ((w + step - 1) / step > maxSide || (h + step - 1) / step > maxSide) ++step;
    const int tw = (w + step - 1) / step, th = (h + step - 1) / step;
    std::vector<std::uint8_t> scaled;
    const std::uint8_t* pixels = rgba;
    if (step > 1) {
        scaled.resize(static_cast<std::size_t>(tw) * th * 4);
        for (int y = 0; y < th; ++y) {
            for (int x = 0; x < tw; ++x) {
                const std::uint8_t* s = rgba + (static_cast<std::size_t>(y * step) * w + x * step) * 4;
                std::copy(s, s + 4, scaled.begin() + (static_cast<std::ptrdiff_t>(y) * tw + x) * 4);
            }
        }
        pixels = scaled.data();
    }
    if (!tex || width != tw || height != th) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, tw, th);
        if (!tex) return false;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        width = tw;
        height = th;
    }
    return SDL_UpdateTexture(tex, nullptr, pixels, tw * 4);
}

Shell::Shell(Session& session, const AppInfo& app, SDL_Window* window, SDL_Renderer* renderer)
    : session_(session), app_(app), window_(window), renderer_(renderer) {
    const auto all = session_.registry().list();
    tools_ = app_.tools;
    if (tools_.empty()) {
        std::set<std::string> seen;
        for (const auto& c : all) {
            std::string prefix = c.id.substr(0, c.id.find('.'));
            if (seen.insert(prefix).second) tools_.push_back(prefix);
        }
    }
    for (const auto& c : all) {
        std::string prefix = c.id.substr(0, c.id.find('.'));
        if (std::find(tools_.begin(), tools_.end(), prefix) != tools_.end()) commands_.push_back(c);
    }
    // a tool this build doesn't carry gets no empty card
    tools_.erase(std::remove_if(tools_.begin(), tools_.end(), [this](const std::string& t) { return toolCommands(t, commands_).empty(); }),
                 tools_.end());
    handoff_ = hasHandoff(session_);
    if (handoff_) {
        self_ = fieldText(session_.run("app.installed", {}), "self");
        refreshInbox();
    }
    if (const auto* paintStroke = find("paint.stroke")) {
        for (const auto& p : paintStroke->params) {
            if (p.name == "brush" && !p.choices.empty()) {
                if (std::find(p.choices.begin(), p.choices.end(), canvas_.brush) == p.choices.end()) canvas_.brush = p.choices.front();
            }
        }
    }
    loadRecent();
    panels_ = std::make_unique<ToolPanels>();
    hasClock_ = find("clock.due") != nullptr;
    loadPalette();
}

Shell::~Shell() {
    if (panels_) panels_->resetTextures();
    canvas_.texture.reset();
}

const CommandInfo* Shell::find(const std::string& id) const {
    for (const auto& c : commands_) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

std::vector<std::string> Shell::docIds() const { return session_.workspace().ids(); }

void Shell::syncDocs() {
    const auto ids = docIds();
    if (!currentDoc_.empty() && std::find(ids.begin(), ids.end(), currentDoc_) == ids.end()) {
        currentDoc_ = ids.empty() ? std::string() : ids.back();
        if (currentDoc_.empty()) showHome_ = true;
    }
    canvas_.dirty = true;
}

Result Shell::run(const std::string& id, const Params& params) {
    Result r = session_.run(id, params);
    if (r.ok) {
        const CommandInfo* info = find(id);
        if (!info || !info->readOnly) ++changes_;
        // a command that made or picked a document: show it
        // most tools answer with doc=; audio projects with project=, songs with song=
        const Value* doc = r.field("doc");
        if (!doc) doc = r.field("project");
        if (!doc) doc = r.field("song");
        if (doc) {
            if (auto s = std::get_if<std::string>(doc); s && session_.workspace().get(*s)) {
                if (*s != currentDoc_) canvas_.fit = true;
                currentDoc_ = *s;
                if (!docTool_.count(*s)) docTool_[*s] = id.substr(0, id.find('.'));
                showHome_ = false;
                panel_ = panelForDoc(*s);  // code and terminals open in their panel
            }
        }
        auto path = params.find("path");
        if (path != params.end() && id.size() > 5 && (id.compare(id.size() - 5, 5, ".open") == 0 || id.compare(id.size() - 5, 5, "_open") == 0)) {
            if (auto p = std::get_if<std::string>(&path->second)) {
                recent_ = pushRecent(recent_, *p);
                saveRecent();
            }
        }
    }
    syncDocs();
    return r;
}

void Shell::select(const std::string& commandId) {
    const CommandInfo* info = find(commandId);
    if (!info) return;
    form_ = buildForm(*info, docIds(), currentDoc_);
    result_.reset();
}

void Shell::runForm() {
    if (!form_) return;
    Params params;
    std::string error;
    resultFor_ = form_->title;
    if (!toParams(*form_, params, error)) {
        result_ = Result::failure(error);
        return;
    }
    Result r = run(form_->command, params);
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
    scrollToResult_ = true;
    // documents may have changed: refresh document pickers to what's open now
    const auto ids = docIds();
    for (auto& f : form_->fields) {
        if (f.kind == FieldKind::Document && (f.text.empty() || std::find(ids.begin(), ids.end(), f.text) == ids.end())) {
            f.text = currentDoc_;
            f.set = !f.text.empty();
        }
    }
}

void Shell::undo() {
    beforeHistoryStep();
    ++changes_;
    if (session_.history().undo()) {
        result_ = Result::success("Undone.");
        resultFor_ = "Undo";
    }
    syncDocs();
}

void Shell::redo() {
    beforeHistoryStep();
    ++changes_;
    if (session_.history().redo()) {
        result_ = Result::success("Redone.");
        resultFor_ = "Redo";
    }
    syncDocs();
}

void Shell::openFile(const std::string& path) {
    if (!app_.launcher.empty()) return launcherOpen(path);  // the launcher hands every file to its own app
    if (openFolder(path)) return;  // a folder opens in the file manager (or as a code project)
    const auto candidates = openCandidates(path, tools_, commands_);
    resultFor_ = "Open a file";
    if (candidates.empty()) {
        result_ = Result::failure("This app can't open files. Try \"New\" instead.");
        suggestOtherApp(path, false);
        if (suggestion_.show) result_ = Result::failure(suggestion_.text);
        return;
    }
    std::string firstError;
    for (const auto& id : candidates) {
        Result r = run(id, {{"path", path}});
        if (r.ok) {
            r.message = sentence(r.message);
            result_ = std::move(r);
            suggestOtherApp(path, true);
            return;
        }
        if (firstError.empty()) firstError = r.message;
    }
    result_ = Result::failure("Couldn't open " + path + ": " + friendlyError(firstError));
    suggestOtherApp(path, false);
    if (suggestion_.show) result_ = Result::failure(suggestion_.text);  // the banner says what to do instead
}

// ---- native file dialogs --------------------------------------------------------

namespace {

// Answers outlive the window on purpose: a dialog may still be open when it closes.
struct DialogQueue {
    std::mutex mutex;
    std::vector<std::pair<DialogTarget, std::string>> results;
    std::string error;
};

DialogQueue& dialogQueue() {
    static DialogQueue q;
    return q;
}

struct DialogRequest {
    DialogTarget target;
};

// SDL may call this on another thread: only queue the answer and wake the loop.
void SDLCALL onDialog(void* userdata, const char* const* files, int) {
    auto* req = static_cast<DialogRequest*>(userdata);
    {
        auto& q = dialogQueue();
        std::lock_guard<std::mutex> lock(q.mutex);
        if (!files) q.error = std::string("Couldn't show a file window (") + SDL_GetError() + "). Type the path into the box instead.";
        else if (files[0]) q.results.emplace_back(req->target, files[0]);
    }
    SDL_Event wake{};
    wake.type = Shell::wakeEvent;
    SDL_PushEvent(&wake);
    delete req;
}

}  // namespace

void Shell::showOpenDialog() {
    auto* req = new DialogRequest{{DialogTarget::OpenDocument, {}, 0}};
    SDL_ShowOpenFileDialog(onDialog, req, window_, nullptr, 0, nullptr, false);
}

void Shell::askFolder(const std::string& purpose) {
    auto* req = new DialogRequest{{DialogTarget::PanelFolder, purpose, 0}};
    SDL_ShowOpenFolderDialog(onDialog, req, window_, nullptr, false);
}

void Shell::showFieldDialog(std::size_t index) {
    if (!form_ || index >= form_->fields.size()) return;
    auto* req = new DialogRequest{{DialogTarget::FormField, form_->command, index}};
    const Field& f = form_->fields[index];
    const char* start = f.text.empty() ? nullptr : f.text.c_str();
    if (f.kind == FieldKind::SaveFile) SDL_ShowSaveFileDialog(onDialog, req, window_, nullptr, 0, start);
    else if (f.kind == FieldKind::Folder) SDL_ShowOpenFolderDialog(onDialog, req, window_, start, false);
    else SDL_ShowOpenFileDialog(onDialog, req, window_, nullptr, 0, start, false);
}

void Shell::drainDialogs() {
    std::vector<std::pair<DialogTarget, std::string>> results;
    std::string error;
    {
        auto& q = dialogQueue();
        std::lock_guard<std::mutex> lock(q.mutex);
        results.swap(q.results);
        error.swap(q.error);
    }
    if (!error.empty()) {
        result_ = Result::failure(error);
        resultFor_ = "File window";
    }
    for (const auto& [target, path] : results) {
        if (target.kind == DialogTarget::OpenDocument) {
            openFile(path);
        } else if (target.kind == DialogTarget::PanelFolder) {
            folderChosen(target.command, path);
        } else if (form_ && form_->command == target.command && target.field < form_->fields.size()) {
            setFieldText(form_->fields[target.field], path);
        }
    }
}

// ---- recent files ------------------------------------------------------------------

void Shell::loadRecent() {
    std::string safe;
    for (char c : app_.name) safe += std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
    char* pref = SDL_GetPrefPath("wetwaresuite", safe.empty() ? "gui" : safe.c_str());
    if (!pref) return;
    recentFile_ = std::string(pref) + "recent.txt";
    SDL_free(pref);
    std::ifstream in(recentFile_, std::ios::binary);
    std::stringstream buf;
    buf << in.rdbuf();
    recent_ = parseRecent(buf.str());
}

void Shell::saveRecent() {
    if (recentFile_.empty()) return;
    std::ofstream out(recentFile_, std::ios::binary | std::ios::trunc);
    out << formatRecent(recent_);
}

}  // namespace wetwaresuite::gui
