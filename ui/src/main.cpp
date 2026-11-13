// QEngine's surface concept: a device's front panel drawn from its layout
// file, to try out how a UI for every emulated device could look. It can play
// a running Engine instance over MIDI, see Midi.hpp, and shows its screen in
// the panel's lcd over VNC, see Screen.hpp.
//
//   qengine-ui [layout name] [--layouts DIR] [--photos DIR] [--instance NAME]
//              [--view drawn|photo|trace] [--screenshot out.png] [--log-input]
//
// --instance connects to that Engine instance's MIDI and screen at start; the sidebar
// does the same for any instance under build/instances.
// --screenshot renders one frame offscreen into a PNG and quits, without
// showing a window, so a layout change can be checked from a script. With
// --instance it waits first for the guest's lamps, four seconds or --wait.
// --log-input prints the pointer, touch and scroll events SDL delivers, to
// see what a touchscreen or a pen actually sends.

#include "Layout.hpp"
#include "Midi.hpp"
#include "Screen.hpp"
#include "Surface.hpp"
#include "Texture.hpp"
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

struct Args {
    std::string device;
    fs::path layouts = QENGINE_UI_LAYOUTS;
    fs::path photos = QENGINE_UI_PHOTOS;
    fs::path instances = QENGINE_UI_INSTANCES;
    std::string instance;
    View view = View::Drawn;
    std::string screenshot;
    double wait = 4.0;  // before a screenshot with a guest connected
    bool logInput = false;
};

bool parseArgs(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;

        if (a == "--layouts" && more) {
            args.layouts = argv[++i];
        } else if (a == "--photos" && more) {
            args.photos = argv[++i];
        } else if (a == "--instance" && more) {
            args.instance = argv[++i];
        } else if (a == "--screenshot" && more) {
            args.screenshot = argv[++i];
        } else if (a == "--wait" && more) {
            args.wait = std::atof(argv[++i]);
        } else if (a == "--view" && more) {
            const std::string v = argv[++i];
            args.view = v == "photo" ? View::Photo : v == "trace" ? View::Trace : View::Drawn;
        } else if (a == "--log-input") {
            args.logInput = true;
        } else if (!a.empty() && a[0] != '-') {
            args.device = a;
        } else {
            std::fprintf(stderr,
                         "usage: %s [layout] [--layouts DIR] [--photos DIR] [--instance NAME] "
                         "[--view drawn|photo|trace] [--screenshot out.png] [--wait SECONDS] [--log-input]\n",
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

// One entry in the device list: a layout file, or one product variant of it.
struct Device {
    fs::path path;
    std::string variant;
    Layout layout;
    std::string error;
};

struct App {
    SDL_Window* window = nullptr;
    SDL_GPUDevice* gpu = nullptr;
    Fonts fonts;
    Args args;

    std::vector<Device> devices;
    int current = 0;
    Surface surface;
    SurfaceOptions options;
    // One per face of the current device. Texture is not movable, hence
    // the pointers.
    std::vector<std::unique_ptr<Texture>> photos;
    std::string photosFor;  // the paths now loaded, to skip reloading them
    double lastReloadCheck = 0;

    // Fingers on the device go to the surface, and one elsewhere drives the
    // sidebar as a mouse would.
    std::unordered_set<uint64_t> surfaceTouches;
    uint64_t uiTouch = 0;
    bool swallowedClick = false;

    // The Engine instance the panel plays, over MIDI, and shows the screen of.
    GuestLink link;
    GuestScreen screen;
    std::vector<Instance> instances;
    int instance = -1;
    double lastInstanceScan = -10;
};

void connectGuest(App& app, const Instance& i) {
    app.link.connect(i.port);

    if (i.screenPort > 0) {
        app.screen.connect(i.screenPort);
    }
}

void disconnectGuest(App& app) {
    app.link.disconnect();
    app.screen.disconnect();
}

// What the screen half of the guest section says.
void screenStatus(const App& app, const Instance& inst) {
    const char* what = inst.engineVnc ? "Engine's VNC" : "QEMU's VNC";

    if (app.screen.connected()) {
        ImGui::TextColored({0.45f, 0.85f, 0.5f, 1}, "Screen, %s", app.screen.status().c_str());
        ImGui::TextDisabled("%s on :%d", what, app.screen.port());
        return;
    }

    ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "Screen waiting for :%d", app.screen.port());
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", app.screen.status().c_str());

    // QEMU only serves VNC for these two; sdl and cocoa open a window instead.
    if (!inst.engineVnc && inst.display != "vnc" && inst.display != "egl-vnc") {
        ImGui::TextWrapped("QEMU serves the screen over VNC only with run_instance.sh --display vnc, or "
                           "egl-vnc, the macOS default with virgl.");
    }

    ImGui::PopStyleColor();
}

// The instance to offer first: the one named on the command line, else one
// built as this device's product.
int pickInstance(const App& app) {
    const Layout& layout = app.devices[size_t(app.current)].layout;

    for (int i = 0; i < int(app.instances.size()); ++i) {
        if (app.instances[size_t(i)].name == app.args.instance) return i;
    }

    for (int i = 0; i < int(app.instances.size()); ++i) {
        const auto& codes = layout.products;
        if (std::find(codes.begin(), codes.end(), app.instances[size_t(i)].product) != codes.end()) return i;
    }

    return app.instances.empty() ? -1 : 0;
}

void guestPanel(App& app) {
    ImGui::TextDisabled("GUEST");

    // Instances come and go as they are built, so the list is read again
    // every few seconds.
    if (ImGui::GetTime() - app.lastInstanceScan > 3.0) {
        const std::string chosen = app.instance >= 0 ? app.instances[size_t(app.instance)].name : "";
        app.instances = findInstances(app.args.instances);
        app.lastInstanceScan = ImGui::GetTime();
        app.instance = -1;

        for (int i = 0; i < int(app.instances.size()); ++i) {
            if (app.instances[size_t(i)].name == chosen) app.instance = i;
        }

        if (app.instance < 0 && !app.devices.empty()) {
            app.instance = pickInstance(app);
        }
    }

    if (app.instances.empty()) {
        ImGui::TextWrapped("No Engine instances in %s.", app.args.instances.string().c_str());
        return;
    }

    auto label = [](const Instance& i) { return i.name + "  " + i.product; };
    ImGui::SetNextItemWidth(-1);

    if (ImGui::BeginCombo("##instance", app.instance >= 0 ? label(app.instances[size_t(app.instance)]).c_str() : "")) {
        for (int i = 0; i < int(app.instances.size()); ++i) {
            if (ImGui::Selectable(label(app.instances[size_t(i)]).c_str(), app.instance == i)) {
                app.instance = i;

                if (app.link.wanted()) {
                    connectGuest(app, app.instances[size_t(i)]);
                }
            }
        }
        ImGui::EndCombo();
    }

    const Instance& inst = app.instances[size_t(std::max(app.instance, 0))];

    if (!app.link.wanted()) {
        if (ImGui::Button("Connect", {-1, 0})) {
            connectGuest(app, inst);
        }
        ImGui::TextDisabled("MIDI on localhost:%d", inst.port);

        if (inst.screenPort > 0) {
            ImGui::TextDisabled("Screen on localhost:%d", inst.screenPort);
        }
    } else {
        if (ImGui::Button("Disconnect", {-1, 0})) {
            disconnectGuest(app);
            return;
        }

        if (app.link.connected()) {
            ImGui::TextColored({0.45f, 0.85f, 0.5f, 1}, "Connected, :%d", app.link.port());

            // A guest with midibridge replays Engine's lamps the moment a
            // client connects, so silence means nothing in it reads the port.
            if (app.link.received() == 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("Nothing from the guest yet. An image built before midibridge has nothing "
                                   "reading this port; rebuild it with new_instance.sh --force.");
                ImGui::PopStyleColor();
            }
        } else {
            ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "Waiting for :%d", app.link.port());

            if (!app.link.problem().empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", app.link.problem().c_str());
                ImGui::PopStyleColor();
            }
        }

        const auto& codes = app.devices[size_t(app.current)].layout.products;

        if (std::find(codes.begin(), codes.end(), inst.product) == codes.end()) {
            ImGui::TextWrapped("This panel is not a %s, so Engine will read its controls as that product's.",
                               inst.product.c_str());
        }

        if (app.screen.wanted()) {
            screenStatus(app, inst);
        }

        ImGui::TextDisabled("%u sent, %u received", app.link.sent(), app.link.received());
        ImGui::BeginChild("##midilog", {-1, 120}, ImGuiChildFlags_Borders);
        ImGui::PushFont(nullptr, 12.0f);

        for (const std::string& line : app.link.log()) {
            ImGui::TextUnformatted(line.c_str());
        }

        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::PopFont();
        ImGui::EndChild();
    }
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

void selectDevice(App& app, int index) {
    app.current = index;
    Device& d = app.devices[size_t(index)];
    app.surface.setLayout(d.error.empty() ? &d.layout : nullptr);

    std::string paths;

    for (const Face& face : d.layout.faces) {
        paths += face.photoFile + "\n";
    }

    if (paths != app.photosFor) {
        SDL_WaitForGPUIdle(app.gpu);
        app.photos.clear();

        for (const Face& face : d.layout.faces) {
            auto texture = std::make_unique<Texture>();

            if (!face.photoFile.empty()) {
                texture->load(app.gpu, (app.args.photos / face.photoFile).string());
            }

            app.photos.push_back(std::move(texture));
        }

        app.photosFor = paths;
    }

    app.options.photos.clear();

    for (const auto& texture : app.photos) {
        app.options.photos.push_back({texture->valid() ? texture->id() : ImTextureID_Invalid, texture->size()});
    }
}

// The photo files a layout names that are not in the photos directory.
std::string missingPhotos(const App& app) {
    std::string missing;

    for (size_t i = 0; i < app.photos.size() && i < app.devices[size_t(app.current)].layout.faces.size(); ++i) {
        const std::string& file = app.devices[size_t(app.current)].layout.faces[i].photoFile;

        if (!file.empty() && !app.photos[i]->valid()) {
            missing += (missing.empty() ? "" : ", ") + file;
        }
    }

    return missing;
}

// Re-reads the current layout when its file changes, so a layout can be
// edited with the app open beside it.
void hotReload(App& app) {
    const double now = ImGui::GetTime();

    if (now - app.lastReloadCheck < 0.4 || app.devices.empty()) {
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

void sidebar(App& app, float width) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize({width, vp->WorkSize.y});
    ImGui::Begin("##sidebar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    ImGui::PushFont(app.fonts.title, 26.0f);
    ImGui::TextUnformatted("QEngine");
    ImGui::PopFont();
    ImGui::TextDisabled("Surface concept");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextDisabled("DEVICE");

    for (int i = 0; i < int(app.devices.size()); ++i) {
        const Device& d = app.devices[size_t(i)];
        std::string codes;

        for (const std::string& code : d.layout.products) {
            codes += (codes.empty() ? "" : " ") + code;
        }

        ImGui::PushID(i);

        if (ImGui::Selectable("##device", app.current == i, 0, {0, 44})) {
            selectDevice(app, i);
        }

        const ImVec2 min = ImGui::GetItemRectMin();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(app.fonts.ui, 16.0f, min + ImVec2(10, 5), ImGui::GetColorU32(ImGuiCol_Text),
                    d.layout.name.c_str());
        dl->AddText(app.fonts.ui, 13.0f, min + ImVec2(10, 25), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    codes.c_str());
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("VIEW");

    bool havePhoto = false;

    for (const auto& texture : app.photos) {
        havePhoto = havePhoto || texture->valid();
    }
    int view = int(app.options.view);
    ImGui::RadioButton("Drawn", &view, int(View::Drawn));
    ImGui::BeginDisabled(!havePhoto);
    ImGui::RadioButton("Photo", &view, int(View::Photo));
    ImGui::RadioButton("Trace over photo", &view, int(View::Trace));
    ImGui::EndDisabled();
    app.options.view = View(view);

    if (!app.devices.empty()) {
        const std::string missing = missingPhotos(app);

        if (!missing.empty()) {
            ImGui::TextWrapped("Missing %s in %s.", missing.c_str(), app.args.photos.string().c_str());
        }
    }

    if (app.options.view == View::Trace) {
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##trace", &app.options.traceAlpha, 0.1f, 1.0f, "drawing %.2f");
    }

    ImGui::Spacing();
    ImGui::Checkbox("Lamps from layout", &app.options.layoutLamps);
    ImGui::Checkbox("Animate meters", &app.options.animateMeters);
    ImGui::Checkbox("Show control ids", &app.options.showIds);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    guestPanel(app);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("CONTROL");

    if (const Element* e = app.surface.hovered()) {
        const ImVec2 at = vec2(e->p, "at");
        ImGui::TextUnformatted(e->id.c_str());
        ImGui::TextDisabled("%s at %.1f, %.1f mm", e->kind.c_str(), at.x, at.y);

        if (e->p.contains("midi")) {
            ImGui::TextDisabled("midi %s", e->p["midi"].dump().c_str());
        }
    } else {
        ImGui::TextDisabled("Hover a control");
    }

    if (!app.devices.empty()) {
        const Device& d = app.devices[size_t(app.current)];

        if (!d.error.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1));
            ImGui::TextWrapped("%s", d.error.c_str());
            ImGui::PopStyleColor();
        }

        // At the bottom, unless the list above has grown into it.
        const float footer = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + 12.0f, ImGui::GetWindowHeight() - footer));
        ImGui::Separator();
        ImGui::TextDisabled("%s", d.path.filename().string().c_str());
        ImGui::TextDisabled("%zu elements, reloads on save", d.layout.elements.size());
    }

    ImGui::End();
}

void frame(App& app) {
    const float side = 270.0f;
    sidebar(app, side);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos + ImVec2(side, 0));
    ImGui::SetNextWindowSize(vp->WorkSize - ImVec2(side, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::Begin("##surface", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    // With a guest connected its lamps are the ones shown.
    if (app.link.connected()) {
        app.options.layoutLamps = true;
    }

    app.options.screen = {};

    if (app.screen.connected() && app.screen.texture() != ImTextureID_Invalid) {
        app.options.screen = {app.screen.texture(), app.screen.size()};
    }

    app.surface.draw(app.options, app.fonts);
    app.link.update(app.surface, ImGui::GetTime());

    // Every frame, so a press dragged off the screen still lets go; only a
    // change is sent.
    const ScreenPointer& sp = app.surface.screenPointer();
    app.screen.pointer(sp.uv, sp.down);
    ImGui::End();
    ImGui::PopStyleVar();
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
        if (app.surface.contains(t.pos)) {
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

    loadDevices(app);

    if (app.devices.empty()) {
        std::fprintf(stderr, "no layouts in %s\n", app.args.layouts.string().c_str());
    } else {
        selectDevice(app, app.current);
    }

    if (!app.args.instance.empty()) {
        for (const Instance& i : findInstances(app.args.instances)) {
            if (i.name == app.args.instance) {
                connectGuest(app, i);
            }
        }

        if (!app.link.wanted()) {
            std::fprintf(stderr, "no Engine instance %s in %s\n", app.args.instance.c_str(),
                         app.args.instances.string().c_str());
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

            if (event.type == SDL_EVENT_MOUSE_WHEEL) {
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
        frame(app);
        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();

        // A few frames first, so fonts are rasterised and the layout settles.
        if (shot) {
            // With a guest, long enough for it to answer, so the shot shows
            // its lamps.
            const bool settled = !app.link.wanted() || ImGui::GetTime() > app.args.wait;

            if (++frames >= 4 && settled) {
                running = false;

                if (app.link.wanted()) {
                    std::printf("guest: %s, %u sent, %u received\n",
                                app.link.connected() ? "connected" : "not connected", app.link.sent(),
                                app.link.received());
                    std::printf("screen: %s, %s\n", app.screen.connected() ? "connected" : "not connected",
                                app.screen.status().c_str());
                    const auto& log = app.link.log();

                    for (size_t i = log.size() > 12 ? log.size() - 12 : 0; i < log.size(); ++i) {
                        std::printf("  %s\n", log[i].c_str());
                    }
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

    disconnectGuest(app);
    SDL_WaitForGPUIdle(app.gpu);
    app.screen.release();
    app.photos.clear();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(app.gpu, app.window);
    SDL_DestroyGPUDevice(app.gpu);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
