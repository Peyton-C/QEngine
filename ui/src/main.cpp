// QEngine's front end, laid out like a VM manager: the instances under
// build/instances down the side, each started and stopped from there, and a
// New Device sheet that builds one. The selected instance is drawn as its
// front panel from a layout file, played into the guest over MIDI, see
// Midi.hpp, with the guest's screen in the panel's lcd over VNC, see
// Screen.hpp. Shell.cpp draws the window and Wizard.cpp the sheet.
//
//   qengine-ui [layout name] [--layouts DIR] [--instance NAME]
//              [--new] [--view drawn|screen] [--screenshot out.png]
//              [--overlay image.png [--overlay-face top|front]
//               [--overlay-rect x0,y0,x1,y1] [--overlay-alpha A]]
//              [--log-input]
//
// A layout name or product code opens that layout on its own, without a
// guest. --instance selects that instance, and connects to it if it runs.
// --new opens the New Device sheet.
// --screenshot renders one frame offscreen into a PNG and quits, without
// showing a window, so a layout change can be checked from a script. With a
// running guest it waits for the screen and lamps, four seconds or --wait.
// --overlay lays a PNG over the top face, or the one --overlay-face names, at
// --overlay-alpha, 0.5 by default, so a layout can be checked against a
// product photo. --overlay-rect is where the face sits in the image, in
// pixels, and is the whole image without it. It has no place in the window's
// menus, since only someone drawing a layout needs it.
// --log-input prints the pointer, touch and scroll events SDL delivers, to
// see what a touchscreen or a pen actually sends.

#include "App.hpp"
#include "Touch.hpp"

#include <SDL3/SDL.h>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include "imgui.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool parseArgs(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;

        if (a == "--layouts" && more) {
            args.layouts = argv[++i];
        } else if (a == "--instance" && more) {
            args.instance = argv[++i];
        } else if (a == "--screenshot" && more) {
            args.screenshot = argv[++i];
        } else if (a == "--wait" && more) {
            args.wait = std::atof(argv[++i]);
        } else if (a == "--view" && more) {
            const std::string v = argv[++i];
            args.view = v == "screen" ? View::Screen : View::Drawn;
        } else if (a == "--overlay" && more) {
            args.overlay = argv[++i];
        } else if (a == "--overlay-face" && more) {
            args.overlayFace = argv[++i];
        } else if (a == "--overlay-alpha" && more) {
            args.overlayAlpha = float(std::atof(argv[++i]));
        } else if (a == "--overlay-rect" && more) {
            ImVec4& r = args.overlayRect;

            if (std::sscanf(argv[++i], "%f,%f,%f,%f", &r.x, &r.y, &r.z, &r.w) != 4 || r.z <= r.x || r.w <= r.y) {
                std::fprintf(stderr, "--overlay-rect wants x0,y0,x1,y1\n");
                return false;
            }
        } else if (a == "--new") {
            args.newDevice = true;
        } else if (a == "--log-input") {
            args.logInput = true;
        } else if (!a.empty() && a[0] != '-') {
            args.device = a;
        } else {
            std::fprintf(stderr,
                         "usage: %s [layout] [--layouts DIR] [--instance NAME] "
                         "[--new] [--view drawn|screen] [--screenshot out.png] [--wait SECONDS] "
                         "[--overlay image.png] [--overlay-face top|front] [--overlay-rect x0,y0,x1,y1] "
                         "[--overlay-alpha A] [--log-input]\n",
                         argv[0]);
            return false;
        }
    }

    return true;
}

// How far, and which way, the finger moved for a scroll, in pixels with y
// down. SDL's y is how far the content should move down, and macOS reports a
// touch or trackpad's precise deltas a tenth of a point at a time. Natural
// scrolling, the macOS default, moves content with the finger, so the
// motion is the scroll itself; otherwise it is the reverse, which also makes
// a mouse wheel rolled away from you turn a knob clockwise.
ImVec2 fingerMotion(const SDL_MouseWheelEvent& w) {
    const float sign = w.direction == SDL_MOUSEWHEEL_FLIPPED ? 1.0f : -1.0f;
    return ImVec2(-w.x, w.y) * (sign * 10.0f);
}

void logInput(const SDL_Event& e) {
    const auto source = [](SDL_MouseID which) {
        return which == SDL_TOUCH_MOUSEID ? "touch" : which == SDL_PEN_MOUSEID ? "pen" : "mouse";
    };

    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        std::printf("button %s %d from %s at %.0f,%.0f\n", e.button.down ? "down" : "up", e.button.button,
                    source(e.button.which), e.button.x, e.button.y);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (e.motion.state) {
            std::printf("drag from %s at %.0f,%.0f\n", source(e.motion.which), e.motion.x, e.motion.y);
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        std::printf("scroll %.2f,%.2f %s from %s\n", e.wheel.x, e.wheel.y,
                    e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? "natural" : "normal", source(e.wheel.which));
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
        std::printf("finger %s %llu at %.3f,%.3f\n",
                    e.type == SDL_EVENT_FINGER_DOWN ? "down" : e.type == SDL_EVENT_FINGER_UP ? "up" : "move",
                    (unsigned long long)e.tfinger.fingerID, e.tfinger.x, e.tfinger.y);
        break;
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
        std::printf("pen %s\n", e.type == SDL_EVENT_PEN_DOWN ? "down" : "up");
        break;
    default:
        return;
    }

    std::fflush(stdout);
}

void logTouch(const TouchEvent& t) {
    std::printf("touch %s %llu at %.0f,%.0f\n",
                t.phase == TouchEvent::Down ? "down" : t.phase == TouchEvent::Up ? "up" : "move",
                (unsigned long long)t.id, t.pos.x, t.pos.y);
    std::fflush(stdout);
}

ImFont* loadFont(std::initializer_list<const char*> paths, float size) {
    ImGuiIO& io = ImGui::GetIO();

    for (const char* path : paths) {
        std::error_code ec;

        if (fs::exists(path, ec)) {
            if (ImFont* font = io.Fonts->AddFontFromFileTTF(path, size)) {
                return font;
            }
        }
    }

    return nullptr;
}

Fonts loadFonts() {
    Fonts f;
    f.ui = loadFont({"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/HelveticaNeue.ttc",
                     "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                     "/usr/share/fonts/TTF/DejaVuSans.ttf"},
                    15.0f);

    if (!f.ui) {
        f.ui = ImGui::GetIO().Fonts->AddFontDefault();
    }

    // Panels are printed in a narrow bold sans, so the labels fit their caps.
    f.label = loadFont({"/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf",
                        "/usr/share/fonts/truetype/liberation/LiberationSansNarrow-Bold.ttf",
                        "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf"},
                       15.0f);
    f.title = loadFont({"/System/Library/Fonts/Supplemental/DIN Alternate Bold.ttf",
                        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
                        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"},
                       15.0f);

    f.mono = loadFont({"/System/Library/Fonts/SFNSMono.ttf", "/System/Library/Fonts/Menlo.ttc",
                       "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                       "/usr/share/fonts/TTF/DejaVuSansMono.ttf"},
                      13.0f);

    if (!f.mono) f.mono = f.ui;

    f.icons = loadFont({QENGINE_UI_ICONS}, 16.0f);

    if (!f.icons) {
        std::fprintf(stderr, "no icon font at %s\n", QENGINE_UI_ICONS);
    }

    setIconFont(f.icons);
    if (!f.label) f.label = f.ui;
    if (!f.title) f.title = f.label;
    return f;
}

void applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.FrameRounding = 5;
    s.GrabRounding = 5;
    s.WindowPadding = {16, 16};
    s.FramePadding = {9, 6};
    s.ItemSpacing = {8, 8};
    s.WindowBorderSize = 0;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = {0.075f, 0.08f, 0.09f, 1};
    c[ImGuiCol_FrameBg] = {0.13f, 0.14f, 0.16f, 1};
    c[ImGuiCol_FrameBgHovered] = {0.17f, 0.18f, 0.21f, 1};
    c[ImGuiCol_Header] = {0.16f, 0.2f, 0.27f, 1};
    c[ImGuiCol_HeaderHovered] = {0.19f, 0.24f, 0.33f, 1};
    c[ImGuiCol_HeaderActive] = {0.21f, 0.28f, 0.38f, 1};
    c[ImGuiCol_CheckMark] = {0.45f, 0.75f, 1.0f, 1};
    c[ImGuiCol_SliderGrab] = {0.45f, 0.75f, 1.0f, 1};
    c[ImGuiCol_Button] = {0.15f, 0.16f, 0.19f, 1};
    c[ImGuiCol_ButtonHovered] = {0.2f, 0.22f, 0.26f, 1};
    c[ImGuiCol_ButtonActive] = {0.21f, 0.28f, 0.38f, 1};
    c[ImGuiCol_Separator] = {0.18f, 0.19f, 0.22f, 1};
}

void loadDevices(App& app) {
    for (const fs::path& path : findLayouts(app.args.layouts)) {
        Device base;
        base.path = path;
        loadLayout(path, base.layout, base.error);
        const std::vector<std::string> variants = base.layout.variants;

        // The base, then each variant, all from the same file.
        std::vector<Device> found;
        found.push_back(std::move(base));

        for (const std::string& code : variants) {
            Device d;
            d.path = path;
            d.variant = code;
            loadLayout(path, d.layout, d.error, code);
            found.push_back(std::move(d));
        }

        for (Device& d : found) {
            // Pick by file name or by product code.
            const auto& codes = d.layout.products;
            const bool named = (d.variant.empty() && path.stem() == app.args.device) ||
                               std::find(codes.begin(), codes.end(), app.args.device) != codes.end();

            app.devices.push_back(std::move(d));

            if (named) {
                app.current = int(app.devices.size()) - 1;
            }
        }
    }
}

}  // namespace

// Draws the layout at `index`, or none for -1.
void selectDevice(App& app, int index) {
    app.current = index;
    const Device* d = index >= 0 ? &app.devices[size_t(index)] : nullptr;
    app.surface.setLayout(d && d->error.empty() ? &d->layout : nullptr);
}

namespace {

// Re-reads the current layout when its file changes, so a layout can be
// edited with the app open beside it.
void hotReload(App& app) {
    const double now = ImGui::GetTime();

    if (now - app.lastReloadCheck < 0.4 || app.current < 0) {
        return;
    }

    app.lastReloadCheck = now;
    Device& d = app.devices[size_t(app.current)];
    std::error_code ec;
    const auto mtime = fs::last_write_time(d.path, ec);

    if (ec || mtime == d.layout.mtime) {
        return;
    }

    Layout fresh;
    std::string error;

    if (loadLayout(d.path, fresh, error, d.variant)) {
        d.layout = std::move(fresh);
        d.error.clear();
    } else {
        // Keep drawing the last good layout, and say why the new one failed.
        d.layout.mtime = mtime;
        d.error = error;
    }

    selectDevice(app, app.current);
}

// Renders the frame into an offscreen texture and writes it out. The
// swapchain is not used, so it works with the window hidden.
bool saveScreenshot(App& app, ImDrawData* drawData, SDL_GPUTextureFormat format, const std::string& path) {
    const Uint32 w = Uint32(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const Uint32 h = Uint32(drawData->DisplaySize.y * drawData->FramebufferScale.y);

    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = format;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(app.gpu, &ti);

    SDL_GPUTransferBufferCreateInfo tb = {};
    tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tb.size = w * h * 4;
    SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(app.gpu, &tb);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(app.gpu);
    app.screen.upload(cmd);
    ImGui_ImplSDLGPU3_PrepareDrawData(drawData, cmd);

    SDL_GPUColorTargetInfo ct = {};
    ct.texture = target;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.clear_color = {0.05f, 0.05f, 0.06f, 1};
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    ImGui_ImplSDLGPU3_RenderDrawData(drawData, cmd, pass);
    SDL_EndGPURenderPass(pass);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src = {};
    src.texture = target;
    src.w = w;
    src.h = h;
    src.d = 1;
    SDL_GPUTextureTransferInfo dst = {};
    dst.transfer_buffer = download;
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(app.gpu, true, &fence, 1);
    SDL_ReleaseGPUFence(app.gpu, fence);

    void* pixels = SDL_MapGPUTransferBuffer(app.gpu, download, false);
    // ARGB8888 is B, G, R, A in memory on little endian, which is what a
    // BGRA swapchain format holds.
    const SDL_PixelFormat pf = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ? SDL_PIXELFORMAT_ARGB8888
                                                                             : SDL_PIXELFORMAT_ABGR8888;
    SDL_Surface* surface = SDL_CreateSurfaceFrom(int(w), int(h), pf, pixels, int(w * 4));
    const bool ok = surface && SDL_SavePNG(surface, path.c_str());

    if (!ok) {
        std::fprintf(stderr, "screenshot: %s\n", SDL_GetError());
    }

    SDL_DestroySurface(surface);
    SDL_UnmapGPUTransferBuffer(app.gpu, download);
    SDL_ReleaseGPUTransferBuffer(app.gpu, download);
    SDL_ReleaseGPUTexture(app.gpu, target);
    return ok;
}

}  // namespace

// SDL's own touchscreen events, where it has them, go the same way as the
// fingers macOS reports to QEngine's recognizer.
void sdlFinger(App& app, const SDL_TouchFingerEvent& f, SDL_EventType type) {
    if (SDL_GetTouchDeviceType(f.touchID) != SDL_TOUCH_DEVICE_DIRECT) {
        return;
    }

    int w = 0;
    int h = 0;
    SDL_GetWindowSize(app.window, &w, &h);
    TouchEvent t;
    t.phase = type == SDL_EVENT_FINGER_DOWN ? TouchEvent::Down
              : type == SDL_EVENT_FINGER_MOTION ? TouchEvent::Move
                                                : TouchEvent::Up;
    t.id = f.fingerID;
    t.pos = ImVec2(f.x * float(w), f.y * float(h));
    touchPush(t);
}

// True for the click macOS makes up after a tap, which the recognizer has
// already reported as a touch. A real mouse used within half a second of a
// finger is dropped too.
bool madeUpClick(App& app, const SDL_Event& e) {
    constexpr uint64_t window = 500'000'000;

    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        app.swallowedClick = e.button.which != SDL_TOUCH_MOUSEID && touchRecent(window);
        return app.swallowedClick;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return std::exchange(app.swallowedClick, false);
    case SDL_EVENT_MOUSE_MOTION:
        return touchRecent(window);
    default:
        return false;
    }
}

void routeTouch(App& app, const TouchEvent& t) {
    ImGuiIO& io = ImGui::GetIO();

    if (t.phase == TouchEvent::Down) {
        // The Screen view draws no panel, so a finger there is a mouse.
        if (!app.settingsOpen && app.options.view != View::Screen && app.surface.contains(t.pos)) {
            app.surfaceTouches.insert(t.id);
        } else if (app.uiTouch == 0) {
            app.uiTouch = t.id;
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(t.pos.x, t.pos.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            return;
        }
    }

    if (app.surfaceTouches.count(t.id)) {
        app.surface.touch(t);

        if (t.phase == TouchEvent::Up) {
            app.surfaceTouches.erase(t.id);
        }
    } else if (t.id == app.uiTouch) {
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(t.pos.x, t.pos.y);

        if (t.phase == TouchEvent::Up) {
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            app.uiTouch = 0;
        }
    }
}

int main(int argc, char** argv) {
    App app;

    if (!parseArgs(argc, argv, app.args)) {
        return 2;
    }

    const bool shot = !app.args.screenshot.empty();
    app.options.view = app.args.view;

    SDL_SetAppMetadata("QEngine", "0.1", "dev.qengine.ui");

    // Touches come as touches, not as a mouse SDL makes up from them.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    if (!shot) {
        app.settingsFile = settingsPath();
        app.settings = Settings::load(app.settingsFile);
    }

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

    if (shot) {
        flags |= SDL_WINDOW_HIDDEN;
    }

    app.window = SDL_CreateWindow("QEngine", 1500, 960, flags);
    app.gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL |
                                      SDL_GPU_SHADERFORMAT_DXIL,
                                  false, nullptr);

    if (!app.window || !app.gpu || !SDL_ClaimWindowForGPUDevice(app.gpu, app.window)) {
        std::fprintf(stderr, "GPU setup: %s\n", SDL_GetError());
        return 1;
    }

    if (!shot && touchStart(app.window) && app.args.logInput) {
        std::printf("listening for direct touches\n");
        std::fflush(stdout);
    }

    SDL_SetGPUSwapchainParameters(app.gpu, app.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                  SDL_GPU_PRESENTMODE_VSYNC);
    const SDL_GPUTextureFormat format = SDL_GetGPUSwapchainTextureFormat(app.gpu, app.window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    applyStyle();
    app.fonts = loadFonts();

    ImGui_ImplSDL3_InitForSDLGPU(app.window);
    ImGui_ImplSDLGPU3_InitInfo init = {};
    init.Device = app.gpu;
    init.ColorTargetFormat = format;
    init.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init);

    if (!app.args.overlay.empty()) {
        if (app.overlay.load(app.gpu, app.args.overlay)) {
            app.options.overlay = {{app.overlay.id(), app.overlay.size()},
                                   app.args.overlayFace,
                                   app.args.overlayRect,
                                   app.args.overlayAlpha};
        } else {
            std::fprintf(stderr, "could not load %s\n", app.args.overlay.c_str());
        }
    }

    loadDevices(app);

    if (app.devices.empty()) {
        std::fprintf(stderr, "no layouts in %s\n", app.args.layouts.string().c_str());
    }

    app.machines = std::make_unique<Machines>(app.args.repo, app.args.instances);
    app.instances = app.machines->list();

    if (!app.args.instance.empty()) {
        selectInstance(app, app.args.instance);

        if (!selectedInstance(app)) {
            std::fprintf(stderr, "no instance %s in %s\n", app.args.instance.c_str(),
                         app.args.instances.string().c_str());
        }
    } else if (app.current >= 0) {
        // A layout named on the command line, on its own.
        app.pane = Pane::Layout;
        app.layoutsOpen = true;
        selectDevice(app, app.current);
    }

    if (app.args.newDevice) {
        wizardOpen(app);

        // qengine-ui JP24 --new starts the sheet on that product.
        if (findProduct(app.args.device)) {
            app.pane = Pane::Welcome;
            app.wizard.product = app.args.device;
        }
    }

    bool running = true;
    int frames = 0;

    while (running) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (app.args.logInput) {
                logInput(event);
            }

            if (event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION ||
                event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) {
                sdlFinger(app, event.tfinger, SDL_EventType(event.type));
            }

            if (madeUpClick(app, event)) {
                continue;
            }

            ImGui_ImplSDL3_ProcessEvent(&event);

            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }

            if (event.type == SDL_EVENT_MOUSE_WHEEL && !app.settingsOpen) {
                app.surface.scroll(fingerMotion(event.wheel));
            }
        }

        for (const TouchEvent& t : touchDrain()) {
            if (app.args.logInput) {
                logTouch(t);
            }

            routeTouch(app, t);
        }

        if (!shot && (SDL_GetWindowFlags(app.window) & SDL_WINDOW_MINIMIZED)) {
            SDL_Delay(10);
            continue;
        }

        app.screen.prepare(app.gpu);
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        hotReload(app);
        drawApp(app);
        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();

        // A few frames first, so fonts are rasterised and the layout settles.
        if (shot) {
            // A VNC-only guest still needs time to deliver its first frame.
            const bool waitingForGuest = app.link.wanted() || app.screen.wanted();
            const bool settled = !waitingForGuest || ImGui::GetTime() > app.args.wait;

            if (++frames >= 4 && settled) {
                running = false;

                if (app.link.wanted()) {
                    std::printf("guest: %s, %u sent, %u received\n",
                                app.link.connected() ? "connected" : "not connected", app.link.sent(),
                                app.link.received());
                    const auto& log = app.link.log();

                    for (size_t i = log.size() > 12 ? log.size() - 12 : 0; i < log.size(); ++i) {
                        std::printf("  %s\n", log[i].c_str());
                    }
                }

                if (app.screen.wanted()) {
                    std::printf("screen: %s, %s\n", app.screen.connected() ? "connected" : "not connected",
                                app.screen.status().c_str());
                }

                if (!saveScreenshot(app, drawData, format, app.args.screenshot)) {
                    return 1;
                }
            }
            continue;
        }

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(app.gpu);
        SDL_GPUTexture* swapchain = nullptr;
        SDL_WaitAndAcquireGPUSwapchainTexture(cmd, app.window, &swapchain, nullptr, nullptr);

        if (swapchain) {
            app.screen.upload(cmd);
            ImGui_ImplSDLGPU3_PrepareDrawData(drawData, cmd);
            SDL_GPUColorTargetInfo target = {};
            target.texture = swapchain;
            target.load_op = SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;
            target.clear_color = {0.05f, 0.05f, 0.06f, 1};
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(drawData, cmd, pass);
            SDL_EndGPURenderPass(pass);
        }

        SDL_SubmitGPUCommandBuffer(cmd);
    }

    saveSettings(app);
    disconnectGuest(app);
    SDL_WaitForGPUIdle(app.gpu);
    app.screen.release();
    app.overlay.reset();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(app.gpu, app.window);
    SDL_DestroyGPUDevice(app.gpu);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
