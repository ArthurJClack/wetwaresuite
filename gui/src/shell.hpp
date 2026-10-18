// Internal: the window's state and panels. Only window.cpp and the panel
// files include this; apps use gui.hpp.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "imgui.h"
#include "wetwaresuite/gui/gui.hpp"
#include "wetwaresuite/gui/handoff.hpp"
#include "wetwaresuite/gui/logic.hpp"
#include "wetwaresuite/gui/palette.hpp"

namespace wetwaresuite::gui {

// An RGBA texture on the renderer, remade when its picture changes.
struct Texture {
    SDL_Texture* tex = nullptr;
    int width = 0, height = 0;       // texture size
    int srcWidth = 0, srcHeight = 0;  // size of the picture it shows (can be larger than the texture)
    void reset();
    bool upload(SDL_Renderer* renderer, const std::uint8_t* rgba, int w, int h);
    ImTextureID id() const { return static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(tex)); }
};

enum class CanvasTool { Move, Brush, Eraser, Fill, Heal, Remove };

// A retouch command waiting to run: it runs on the frame after it is queued, so the
// window first shows "Working..." over the canvas.
struct PendingRetouch {
    std::string command;
    std::string title;
    Params params;
    bool shown = false;  // a frame with the busy sign has been drawn
};

struct Canvas {
    Texture texture;
    std::string shownDoc;
    bool dirty = true;
    bool fit = true;  // fit to the view on the next frame
    float zoom = 1.0f;
    ImVec2 pan{0, 0};       // offset of the image's top-left from the view's top-left, in screen px
    ImVec2 viewMin{0, 0};   // last frame's view rectangle
    ImVec2 viewSize{0, 0};
    std::string summary;  // for documents with no picture

    CanvasTool tool = CanvasTool::Move;
    std::string brush = "round";
    float color[4] = {0.1f, 0.1f, 0.1f, 1.0f};
    float size = 12;
    float opacity = 100;
    bool stroking = false;
    bool penErasing = false;
    bool usedPen = false;
    std::vector<double> points;     // image px, x,y pairs
    std::vector<double> pressures;  // one per point when a pen is used

    // retouching (retouch.cpp)
    HealSource healSource;       // Alt+click spot the Heal tool copies from
    bool pickingSource = false;  // "Pick source" is on: the next click picks the spot
    std::vector<double> removeMark;  // painted area waiting for "Remove", picture px
    float removeMarkSize = 0;
    std::string removeMarkDoc;
    bool autoRemove = false;  // remove as soon as the stroke ends
    std::optional<PendingRetouch> pending;
};

// Where a native file dialog's answer goes.
struct ToolPanels;  // tool_panels.hpp

struct DialogTarget {
    enum Kind { OpenDocument, FormField, PanelFolder } kind = OpenDocument;
    std::string command;  // FormField: the form it belongs to; PanelFolder: what the folder is for
    std::size_t field = 0;
};

class Shell {
public:
    Shell(Session& session, const AppInfo& app, SDL_Window* window, SDL_Renderer* renderer);
    ~Shell();

    void applyStyle(float dpiScale);
    void frame();                         // one ImGui frame of every panel
    void onEvent(const SDL_Event& event);  // raw input for strokes, before ImGui sees it
    void openFile(const std::string& path);
    // a command's form, or "settings" for the Aa menu (text size and colours)
    void showAction(const std::string& commandId) {
        if (commandId == "settings") { openSettings_ = true; return; }
        select(commandId);
        openPanelFor(commandId);
    }
    bool wantsQuit() const { return quit_; }
    int tickMs() const;  // > 0: wake the window this often (timers, running programs, live graphs)

    static Uint32 wakeEvent;  // pushed from dialog threads to wake the event loop
    static ImFont* monoFont;  // a fixed-width font for code and terminals (null: the UI font)

private:
    // panels (panels.cpp)
    void topBar();
    void leftPanel();
    void rightPanel();
    void homeView();
    void documentView();
    void formPanel();
    void historyPanel();
    void searchPopup();
    void newPopup();
    void settingsPopup();
    void coloursSettings();  // colours.cpp

    // the shared colour palette (colours.cpp), through the app.palette_* commands
    bool loadPalette();  // true when the colours changed
    void savePaletteNow();
    void runPalette(const std::string& command, const Params& params);
    bool darkLook() const { return custom_ ? theme_.dark : dark_; }
    void field(Field& f, std::size_t index);

    // canvas (canvas.cpp)
    void canvasView();
    void canvasToolbar();
    void refreshCanvas();
    void beginStroke(ImVec2 screen, float pressure, bool pen, bool eraser);
    void addStrokePoint(ImVec2 screen, float pressure);
    void endStroke();
    ImVec2 toImage(ImVec2 screen) const;
    bool toolAvailable(CanvasTool tool) const;

    // retouching by painting (retouch.cpp): Heal and Remove
    void retouchSettings();
    bool retouchPress(ImVec2 screen);  // true when the click picked a heal source instead of starting a stroke
    void retouchRelease();
    void drawRetouch(ImDrawList* dl, ImVec2 origin, bool hovered);
    void applyRemove();
    void queueRetouch(const std::string& command, const std::string& title, Params params);
    void runPendingRetouch();
    bool pictureSize(int& width, int& height) const;

    // actions (shell.cpp)
    Result run(const std::string& id, const Params& params);
    void select(const std::string& commandId);
    void runForm();
    void undo();
    void redo();
    void showOpenDialog();
    void showFieldDialog(std::size_t index);
    void drainDialogs();
    void loadRecent();
    void saveRecent();
    void syncDocs();
    std::vector<std::string> docIds() const;
    const CommandInfo* find(const std::string& id) const;
    bool accentButton(const char* label, ImVec2 size = {0, 0});

    // tool panels (tool_panels.cpp, panel_*.cpp): the calculator, code editor,
    // file manager, terminal, system monitor and clock fill the middle
    bool hasPanel(const std::string& tool) const;
    std::string panelForDoc(const std::string& id) const;
    void openPanel(const std::string& tool);
    void openPanelFor(const std::string& commandId);
    bool openFolder(const std::string& path);
    void panelList();
    void toolPanelView();
    void backgroundTicks();
    void beforeHistoryStep();
    void askFolder(const std::string& purpose);
    void folderChosen(const std::string& purpose, const std::string& path);
    Result query(const std::string& id, const Params& params);  // polls: no history, no switching documents
    void calcPanel();
    void calcKeypad();
    void calcSide();
    void calcEvaluate();
    void calcGraph();
    void codePanel();
    void codeToolbar();
    void codeSidebar();
    void codeTree();
    void codeOutline();
    void codeSearch();
    void codeGit();
    void codeTabs();
    void codeFindBar();
    void codeOutput();
    void codeSync();
    void codeCommit();
    void codeOpenAt(const std::string& path, int line);
    void codeRun(const std::string& what);
    void codePoll();
    void codeFind(int step);
    void explorerPanel();
    void explorerToolbar();
    void explorerSide();
    void explorerList();
    void explorerPreview();
    void explorerPopups();
    void explorerTrash();
    void explorerArchive();
    void explorerGo(const std::string& folder, bool remember = true);
    void explorerOpen(const std::string& path, bool folder);
    void explorerRefresh();
    void terminalPanel();
    void terminalNew(const std::string& folder);
    void terminalPoll(bool force);
    void monitorPanel();
    void monitorSample();
    void clockPanel();
    void clockTick();
    void clockTimers();
    void clockStopwatch();
    void clockAlarms();
    void clockDates();
    void dueBanner();

    // handing work to other apps (handoff_panels.cpp), all through the app.* commands
    void refreshInbox();
    void sendButton();
    void sendPopup();
    void inboxBannerView();
    void suggestionBannerView();
    void launchOfferView();
    void suggestOtherApp(const std::string& path, bool openedHere);
    void sendTo(const std::string& app);
    void launchApp(const std::string& app, const std::string& path);

    Session& session_;
    AppInfo app_;
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    std::vector<std::string> tools_;
    std::vector<CommandInfo> commands_;  // this app's commands

    bool pro_ = false;
    bool dark_ = true;  // the Dark/Light choice for the app's own colours
    bool custom_ = false;  // a palette was picked: every colour comes from theme_
    Palette palette_;
    Theme theme_;
    bool paletteStale_ = false;    // re-read the palette on the next frame (the window got focus)
    bool palettePending_ = false;  // a picker changed it: save once the mouse is let go
    std::string paletteNote_;      // the last palette command's answer or error
    bool paletteNoteOk_ = true;
    bool openSettings_ = false;
    ImVec4 accent_{0.24f, 0.49f, 1, 1}, accentHover_, accentActive_, accentText_{1, 1, 1, 1}, title_;
    float textScale_ = 1.0f;
    float dpi_ = 1.0f;
    bool quit_ = false;
    bool showHome_ = true;
    std::string currentDoc_;
    std::map<std::string, std::string> docTool_;  // doc id -> tool that made it

    std::string query_;
    bool focusSearch_ = false;
    int searchPick_ = 0;
    ImVec2 searchMin_{0, 0}, searchMax_{0, 0};
    bool searchActive_ = false;
    bool searchHovered_ = false;
    std::string expandTool_;

    std::optional<Form> form_;
    std::optional<Result> result_;
    std::string resultFor_;
    bool scrollToResult_ = false;

    Canvas canvas_;
    float penPressure_ = 1.0f;
    bool penDown_ = false;
    bool penEraser_ = false;
    std::vector<std::string> recent_;
    std::string recentFile_;

    bool handoff_ = false;  // the session has app.send / app.inbox / ...
    std::string self_;      // this app's id from app.installed ("suite" for the all-in-one)
    InboxBanner inbox_;
    OpenSuggestion suggestion_;
    std::vector<SendChoice> sendChoices_;
    std::string launchOffer_;  // app to offer to start after a send

    std::unique_ptr<ToolPanels> panels_;
    std::string panel_;          // tool whose panel fills the middle; empty = home or a document
    std::uint64_t changes_ = 0;  // goes up on every change, so panels know to reload
    bool hasClock_ = false;
};

}  // namespace wetwaresuite::gui
