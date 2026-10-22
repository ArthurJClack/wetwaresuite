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

enum class CanvasTool { Move, Select, Brush, Eraser, Fill, Heal, Remove, Clone, Dodge, Burn, Sponge, Smudge, Blur, Pick, Anchor };
enum class SelectShape { Rect, Ellipse, Lasso, Wand };

// Click tools for game levels (levels.*) and pixel-art sprites (forge.sprite_*), game_view.cpp.
enum class GameTool { Move, Paint, Rect, Erase, Fill, Object, Pick, Pencil, Eraser, Line, Box, Ellipse, Bucket };

struct GameCanvas {
    std::string doc, kind;       // the document the state is for; kind "level" or "sprite"
    std::uint64_t seen = ~0ull;  // Shell change count the info was read at
    GameTool tool = GameTool::Move;
    bool dragging = false;
    ImVec2 dragFrom{0, 0}, dragTo{0, 0};  // picture px
    std::vector<double> points;           // level: cells x,y; sprite: pixels x,y
    std::string error;
    // level
    int mapW = 0, mapH = 0, tileW = 16, tileH = 16;
    std::vector<std::string> layers, layerKinds, tilesets;
    int layer = 0, tileset = 0, tile = 0;
    Texture tilesetTex;
    std::string tilesetFor;  // doc/tileset/change the texture shows
    int tsColumns = 1, tsTiles = 0, tsTileW = 16, tsTileH = 16;
    bool grid = true;
    // sprite
    int frame = 0, frames = 1, spriteLayer = 0, width = 0, height = 0;
    std::vector<std::string> spriteLayers, palette, tags, tagRanges;
    std::vector<bool> layerShown;
    std::vector<int> durations;
    bool onion = false, playing = false, filled = false;
    double playClock = 0;
    float color[4] = {0, 0, 0, 1};
    std::vector<std::uint8_t> picture;  // the frame on the canvas, RGBA, for the colour picker
    int pictureW = 0, pictureH = 0;
};

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
    // retouch brushes (brush_tools.cpp): clone, dodge/burn/sponge, smudge, blur/sharpen
    HealSource cloneSource;   // Alt+click spot the clone stamp copies from
    float strength = 50;      // exposure / strength 0..100
    std::string toneRange = "midtones";
    bool sharpen = false;     // the Blur tool sharpens instead

    // the 3D view (orbit.cpp), for documents that can draw themselves from any angle
    bool is3d = false;
    std::string shown3dDoc;
    OrbitView orbit;
    bool orbitDirty = true;
    bool orbitDragging = false;
    bool orbitSharpPending = false;
    int orbitW = 0, orbitH = 0;

    // selecting (selection_view.cpp)
    SelectShape selectShape = SelectShape::Rect;
    float tolerance = 32;
    bool contiguous = true;
    bool selecting = false;
    ImVec2 selectStart{0, 0};          // picture px
    std::vector<double> lasso;         // picture px, x,y pairs
    int version = 0;                   // goes up whenever the picture is refreshed
    std::string antsDoc;
    int antsVersion = -1;
    int antsStep = 1;                  // picture px per mask cell the outline was traced on
    std::vector<float> ants;           // selection outline, x0,y0,x1,y1 in picture px

    // vector drawings (vector_view.cpp): the picked shape, its box and anchors, in drawing units
    std::string vecSel;
    std::uint64_t vecLoaded = ~0ull;  // changes_ when the box and anchors were read
    bool vecHasBox = false;
    float vecBox[4] = {0, 0, 0, 0};  // x, y, w, h
    std::vector<float> vecNodes;      // per anchor: x, y, in x, in y, out x, out y
    int vecDrag = 0;                  // 0 none, 1 move, 2 scale handle, 3 anchor part
    int vecHandle = 0;                // scale: handle 0..7; anchor: node * 3 + part (0 anchor, 1 in, 2 out)
    ImVec2 vecStart{0, 0}, vecNow{0, 0};

    GameCanvas game;
};

// The play bar's view of playback (transport.cpp), read back from <tool>.playhead.
struct Transport {
    std::string tool;  // audio, beats or cut
    std::string doc;   // the document the window last started
    std::string source;
    bool playing = false, paused = false, loop = false, silent = false;
    double position = 0, length = 0, start = 0, end = 0;
    double cue = 0;  // where Play starts when nothing is playing
    float volumeDb = 0;
};

// The timeline panel's own state (timeline_view.cpp).
struct TimelineState {
    std::string doc;
    double zoom = 0;    // px per second, 0 = fit
    double scroll = 0;  // seconds at the left edge
    double cursor = 0;  // where Split cuts
    std::string selTrack, selClip;
    enum class Drag { None, Cursor, Move, TrimStart, TrimEnd } drag = Drag::None;
    double grabT = 0, origStart = 0, origLength = 0, headroom = -1, tailroom = -1;
};

// The cell grid shown for spreadsheet documents (grid_view.cpp).
struct GridState {
    std::string doc;
    int sheet = -1;  // -1: start on the document's active sheet
    int row = 0, col = 0;
    bool editing = false;
    bool focusEdit = false;   // give the cell editor keyboard focus next frame
    bool caretToEnd = false;  // started by typing: keep the typed text, caret after it
    bool scrollToSel = false;
    bool textInput = false;  // we turned SDL text input on
    std::string buffer;   // the cell editor's text
    std::string formula;  // the formula bar's text
    int formulaRow = -1, formulaCol = -1, formulaSheet = -1;
};

// Clicking on a circuit document (panel_circuit.cpp): the tool and a wire/trace being drawn.
struct CircuitCanvas {
    std::string doc, view = "schematic";  // schematic, pcb or 3d
    std::string tool = "select";          // select, place, wire, move, delete, trace, via
    std::string symbol = "resistor", value, layer = "F";
    bool hasStart = false;  // the first click of a wire or trace
    double startX = 0, startY = 0;  // model point (grid units or mm)
    ImVec2 startPx{0, 0};           // picture px
    std::string moving;             // part picked up by the move tool
};

// Clicking on a map document (maps_view.cpp): the tool, points being drawn, a feature being moved.
struct MapsCanvas {
    std::string doc;
    std::string tool = "identify";  // pan, identify, measure, point, line, area, move, delete, route
    std::string layer = "drawn", props, profile = "car", search, bookmark;
    std::vector<double> points;     // lat,lon pairs clicked so far
    std::vector<ImVec2> pointsPx;   // the same, picture px
    std::string movingLayer;        // move tool: what was picked up
    std::int64_t movingIndex = -1, movingVertex = -1;
    ImVec2 movingFrom{0, 0};
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
    bool animating() const { return animating_; }  // something on screen moves (the selection outline)

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
    void canvasArea();
    void canvasToolbar();
    void refreshCanvas();
    void beginStroke(ImVec2 screen, float pressure, bool pen, bool eraser);
    void addStrokePoint(ImVec2 screen, float pressure);
    void endStroke();
    ImVec2 toImage(ImVec2 screen) const;
    bool toolAvailable(CanvasTool tool) const;
    // circuit documents (panel_circuit.cpp): schematic / board / 3D, and click tools that run circuit.* commands
    bool circuitDoc() const;
    bool circuitEditing() const;  // a click tool is on, so a left drag doesn't pan
    void circuitToolbar();
    void circuitCanvas(ImDrawList* dl, ImVec2 origin, bool hovered, bool clicked);
    void circuitClick(ImVec2 picture);
    // map documents (maps_view.cpp): click tools that run maps.* commands, zoom, bookmarks and search
    bool mapsDoc() const;
    bool mapsEditing() const;  // a click tool is on, so a left drag doesn't pan
    void mapsToolbar();
    void mapsCanvas(ImDrawList* dl, ImVec2 origin, bool hovered, bool clicked);
    void mapsClick(ImVec2 picture);
    void mapsFinish();

    // the 3D view (orbit.cpp)
    bool probe3d(const Document& doc) const;
    void orbitToolbar();
    void orbitCanvas(ImDrawList* dl, ImVec2 origin, ImVec2 size, bool hovered, bool active);

    // retouching by painting (retouch.cpp): Heal and Remove
    void retouchSettings();
    bool retouchPress(ImVec2 screen);  // true when the click picked a heal source instead of starting a stroke
    void retouchRelease();
    void drawRetouch(ImDrawList* dl, ImVec2 origin, bool hovered);
    void applyRemove();
    void queueRetouch(const std::string& command, const std::string& title, Params params);
    void runPendingRetouch();
    bool pictureSize(int& width, int& height) const;
    // the retouch brushes (brush_tools.cpp) and the layers panel (layers_view.cpp)
    static bool brushTool(CanvasTool tool);
    void brushToolSettings();
    bool brushToolPress(ImVec2 screen);  // true when the click picked a clone source
    void runBrushTool();
    void drawBrushTool(ImDrawList* dl, ImVec2 origin, bool hovered);
    void layersPanel();
    void soundPanel();     // effects rack and envelope of audio projects and songs (sound_view.cpp)
    void pianoRollView();  // the notes of a song pattern (sound_view.cpp)

    // the play bar over sound documents (transport.cpp)
    bool playTarget(std::string& tool, Params& target) const;
    void transportBar();
    void pollTransport();
    void startPlaying(double from);
    void togglePlay();
    void seekTo(double seconds);
    // the timeline under video edits and audio projects (timeline_view.cpp)
    float timelineHeight(const TimelineLayout& layout) const;
    void timelineView(const TimelineLayout& layout);
    void runTimelineEdit(const TimelineLayout& layout, const TimelineEdit& edit);
    // spreadsheet grid (grid_view.cpp)
    void gridView(const CellGrid& layout);
    // the PDF editor (pdf_view.cpp)
    void pdfView();
    void pdfToolbar();
    void pdfThumbs(float width);
    void pdfPage();
    void pdfPopup();
    void pdfLoad(bool force);
    void pdfRun(const std::string& command, Params params, const std::string& title);
    void pdfRelease();
    bool commitCell(const CellGrid& layout, int sheet, int row, int col, const std::string& input);
    // selections on the canvas (selection_view.cpp)
    void selectSettings();
    void selectPress(ImVec2 screen);
    void selectDrag(ImVec2 screen);
    void selectRelease(ImVec2 screen);
    void drawSelection(ImDrawList* dl, ImVec2 origin, bool hovered);
    void runSelect(Params params, const char* title);
    // vector drawings on the canvas (vector_view.cpp): pick, move, scale, anchors, align
    bool vectorDoc() const;
    void vectorSettings();
    void vectorPress(ImVec2 screen);
    void vectorDrag(ImVec2 screen);
    void vectorRelease();
    void vectorKeys();
    void drawVector(ImDrawList* dl, ImVec2 origin);

    // game levels and sprites on the canvas (game_view.cpp)
    bool gameActive() const;  // the current document is a level or sprite with its commands here
    void gameSync();          // re-read the document's layers, tilesets, frames, palette
    std::string gamePicture();  // PNG for the canvas, empty when not a game document
    void gameToolbar();
    void gameSide();
    void gameInput(ImDrawList* dl, ImVec2 origin, bool hovered, bool active);
    bool gamePainting() const;
    void gameRun(const std::string& id, Params params, const std::string& title);
    void gameLevelSide();
    void gameSpriteSide();
    void gameLevelRelease();
    void gameSpriteRelease();

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

    // the launcher (launcher_view.cpp): a grid of apps, each started as its own program
    void launcherFrame();
    void launcherOpen(const std::string& path);
    void launcherStart(const std::string& app, const std::string& path);

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
    bool animating_ = false;
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
    Transport transport_;
    float playheadFraction_ = -1;  // where to draw the playhead over a waveform, 0..1
    TimelineState timeline_;
    GridState grid_;
    CircuitCanvas circuit_;
    MapsCanvas maps_;
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
    std::string installed_;      // the launcher: comma list from app.installed
    bool installedStale_ = true;  // re-read it (the window got focus: the hub may have added an app)
    std::string launcherNote_;   // the launcher's last answer
    bool launcherNoteOk_ = true;

    std::unique_ptr<ToolPanels> panels_;
    std::string panel_;          // tool whose panel fills the middle; empty = home or a document
    std::uint64_t changes_ = 0;  // goes up on every change, so panels know to reload
    bool hasClock_ = false;
};

}  // namespace wetwaresuite::gui
