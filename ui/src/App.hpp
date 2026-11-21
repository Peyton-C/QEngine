#pragma once

#include "Catalog.hpp"
#include "Layout.hpp"
#include "Machines.hpp"
#include "Midi.hpp"
#include "Screen.hpp"
#include "Surface.hpp"
#include "Texture.hpp"

#include <SDL3/SDL.h>

#include "imgui.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

struct Args {
    std::string device;  // a layout to open, by file name or product code
    std::filesystem::path layouts = QENGINE_UI_LAYOUTS;
    std::filesystem::path photos = QENGINE_UI_PHOTOS;
    std::filesystem::path repo = QENGINE_UI_REPO;
    std::filesystem::path instances = QENGINE_UI_INSTANCES;
    std::string instance;
    View view = View::Drawn;
    std::string screenshot;
    double wait = 4.0;  // before a screenshot with a guest connected
    bool logInput = false;
    bool newDevice = false;
};

// One layout file, or one product variant of it.
struct Device {
    std::filesystem::path path;
    std::string variant;
    Layout layout;
    std::string error;
};

// What the main area shows: the welcome page, an instance, or a layout on
// its own, which is how a layout is drawn and checked without a guest.
enum class Pane { Welcome, Instance, Layout };

// The New Device sheet, which is also the Edit sheet for a built instance.
struct Wizard {
    bool open = false;
    bool edit = false;  // rebuilding `name`, which is fixed
    int step = 0;

    std::string product;
    std::string firmware;  // absolute
    std::string name;
    bool nameEdited = false;  // else it follows the product and firmware
    bool montero = false;
    char search[64] = {};
    std::string error;

    // The file dialog answers on a thread of its own.
    std::mutex browseMutex;
    std::string browsed;
    bool browsedReady = false;

    Surface preview;
    int previewDevice = -2;
};

struct App {
    SDL_Window* window = nullptr;
    SDL_GPUDevice* gpu = nullptr;
    Fonts fonts;
    Args args;

    std::vector<Device> devices;
    int current = -1;  // the layout drawn, or -1 for none
    Surface surface;
    SurfaceOptions options;
    // One per face of the current device. Texture is not movable, hence
    // the pointers.
    std::vector<std::unique_ptr<Texture>> photos;
    std::string photosFor;  // the paths now loaded, to skip reloading them
    double lastReloadCheck = 0;

    // Fingers on the device go to the surface, and one elsewhere drives the
    // rest of the window as a mouse would.
    std::unordered_set<uint64_t> surfaceTouches;
    uint64_t uiTouch = 0;
    bool swallowedClick = false;

    std::unique_ptr<Machines> machines;
    std::vector<Instance> instances;  // this frame's copy
    Pane pane = Pane::Welcome;
    std::string selected;  // the instance shown

    // The selected instance, played over MIDI and shown over VNC while it
    // runs, and which one that is.
    GuestLink link;
    GuestScreen screen;
    std::string linked;
    bool screenHeld = false;  // a press on a screen drawn without a panel

    bool console = false;
    int consoleTab = 0;
    LogTail consoleTail;
    LogTail buildTail;
    bool layoutsOpen = false;

    std::string confirmDelete;
    std::string message;  // a failure to show for a few seconds
    double messageAt = 0;

    Wizard wizard;
};

// Shell.cpp: the window, the sidebar and what the selected instance shows.
void drawApp(App& app);
void disconnectGuest(App& app);
void selectDevice(App& app, int index);
int layoutFor(const App& app, const std::string& product);
const Instance* selectedInstance(const App& app);
void selectInstance(App& app, const std::string& name);
void showMessage(App& app, const std::string& text);

// Wizard.cpp: the New Device sheet.
void wizardOpen(App& app, const Instance* edit = nullptr);
void wizardDraw(App& app);

// Drawing shared by both.
enum class Icon { Play, Stop, Plus, More, Console, Eye, Folder, Book, Refresh, Back };

std::string familyOf(const Instance& i);
ImU32 familyColour(const std::string& family, float alpha = 1.0f);
void drawIcon(ImDrawList* dl, Icon icon, ImVec2 centre, float size, ImU32 col);
void deviceIcon(ImDrawList* dl, const App& app, ImVec2 min, float size, const std::string& family,
                const std::string& code);
bool iconButton(const char* id, Icon icon, const char* tip, bool on = false, float size = 32.0f);
bool pillButton(const char* label, Icon icon, bool primary, bool danger = false);
void spinner(ImDrawList* dl, ImVec2 centre, float radius, ImU32 col);
std::string productName(const std::string& code);
