// The window: SDL3 + SDL_Renderer (GPU when there is one, software when not)
// with Dear ImGui on top. It waits for input instead of redrawing all the
// time, so an idle window uses next to no CPU.
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>

#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "shell.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::gui {

// The embedded UI font (Roboto Medium, Apache-2.0), generated at configure time.
extern const unsigned char kUiFont[];
extern const unsigned int kUiFontSize;
// The app icon (branding/png/icon-128.png), embedded the same way.
extern const unsigned char kAppIcon[];
extern const unsigned int kAppIconSize;

bool available() { return true; }

bool canOpenWindow(std::string& why) {
    // a quick probe: video starts only when there is a screen (or SDL_VIDEO_DRIVER=offscreen)
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        why = std::string("there is no screen to show it on (") + SDL_GetError() + ")";
        SDL_Quit();
        return false;
    }
    SDL_Quit();
    return true;
}

int run(Session& session, const AppInfo& app) { return run(session, app, {}); }

namespace {

bool saveScreenshot(SDL_Renderer* renderer, const std::string& path) {
    SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr);
    if (!shot) return false;
    bool ok = SDL_SaveBMP(shot, path.c_str());
    SDL_DestroySurface(shot);
    return ok;
}

void setWindowIcon(SDL_Window* window) {
    try {
        raster::Image icon = raster::decode(std::string(reinterpret_cast<const char*>(kAppIcon), kAppIconSize));
        SDL_Surface* surface = SDL_CreateSurfaceFrom(icon.width(), icon.height(), SDL_PIXELFORMAT_RGBA32,
                                                     icon.data(), icon.width() * 4);
        if (!surface) return;
        SDL_SetWindowIcon(window, surface);  // SDL copies the pixels
        SDL_DestroySurface(surface);
    } catch (const std::exception&) {
        // no icon is fine: the window still works
    }
}

}  // namespace

int run(Session& session, const AppInfo& app, const RunOptions& options) {
    SDL_SetHint(SDL_HINT_APP_NAME, app.name.c_str());
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::cerr << "Couldn't start the window: " << SDL_GetError()
                  << "\nEverything still works from the command line: run `wetwaresuite help`.\n";
        return 1;
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1.0f;
    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    SDL_Window* window = SDL_CreateWindow(app.name.c_str(), static_cast<int>(1360 * scale), static_cast<int>(860 * scale), flags);
    if (!window) {
        std::cerr << "Couldn't open a window: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }
    setWindowIcon(window);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
    if (!renderer) {
        std::cerr << "Couldn't draw in the window: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);
    Shell::wakeEvent = SDL_RegisterEvents(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // the layout is fixed: nothing to remember
    io.ConfigInputTextCursorBlink = false;  // blinking would need constant redraws
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kUiFont), static_cast<int>(kUiFontSize), 20.0f, &fc);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    int code = 0;
    {
        Shell shell(session, app, window, renderer);
        shell.applyStyle(scale);
        for (const auto& path : options.open) shell.openFile(path);
        if (!options.action.empty()) shell.showAction(options.action);

        int framesLeft = 3;  // keep drawing a few frames after input so hover and layout settle
        int frameCount = 0;
        bool running = true;
        while (running) {
            SDL_Event e;
            bool got = false;
            if (framesLeft <= 0 && options.screenshot.empty()) got = SDL_WaitEvent(&e);
            else got = SDL_PollEvent(&e);
            while (got) {
                shell.onEvent(e);
                ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type == SDL_EVENT_QUIT) running = false;
                if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window)) running = false;
                if (e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) shell.applyStyle(SDL_GetWindowDisplayScale(window));
                framesLeft = 3;
                got = SDL_PollEvent(&e);
            }
            if (!running) break;
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                framesLeft = 0;
                continue;
            }
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            shell.frame();
            ImGui::Render();
            SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            SDL_SetRenderDrawColorFloat(renderer, bg.x, bg.y, bg.z, 1.0f);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            ++frameCount;
            if (!options.screenshot.empty() && frameCount >= 6) {
                SDL_SetRenderScale(renderer, 1, 1);
                if (!saveScreenshot(renderer, options.screenshot)) {
                    std::cerr << "Couldn't save the screenshot: " << SDL_GetError() << "\n";
                    code = 1;
                }
                running = false;
            }
            SDL_RenderPresent(renderer);
            --framesLeft;
            if (shell.wantsQuit()) running = false;
        }
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return code;
}

}  // namespace wetwaresuite::gui
