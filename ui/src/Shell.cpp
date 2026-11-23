// The window, laid out the way a VM manager is: the instances down the left,
// each with a button to start or stop it, and the selected one filling the
// rest, drawn as its front panel with the guest's screen in it. The plus
// button opens the New Device sheet, Wizard.cpp, which builds a new one.

#include "App.hpp"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fs = std::filesystem;

namespace {

constexpr ImVec4 kGreen = {0.40f, 0.82f, 0.48f, 1};
constexpr ImVec4 kAmber = {0.96f, 0.74f, 0.30f, 1};
constexpr ImVec4 kRed = {1.00f, 0.45f, 0.40f, 1};
constexpr ImVec4 kAccent = {0.30f, 0.56f, 1.00f, 1};

ImU32 u32(ImVec4 c, float alpha = 1.0f) {
    c.w *= alpha;
    return ImGui::GetColorU32(c);
}

const char* stateName(const Instance& i) {
    switch (i.state) {
    case MachineState::Running: return "Running";
    case MachineState::Stopping: return "Stopping";
    case MachineState::Building: return "Building";
    case MachineState::Failed: return i.ready ? "Rebuild failed" : "Build failed";
    default: return "Stopped";
    }
}

ImVec4 stateColour(const Instance& i) {
    switch (i.state) {
    case MachineState::Running: return kGreen;
    case MachineState::Stopping:
    case MachineState::Building: return kAmber;
    case MachineState::Failed: return kRed;
    default: return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    }
}

std::string version(const Instance& i) {
    return firmwareVersion(fs::path(i.firmware).filename().string());
}

void openPath(const fs::path& path) {
    SDL_OpenURL(("file://" + path.string()).c_str());
}

// Text cut to fit, with an ellipsis.
void clippedText(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 col, const std::string& text,
                 float width) {
    ImGui::PushFont(font, size);
    std::string s = text;

    if (ImGui::CalcTextSize(s.c_str()).x > width) {
        while (!s.empty() && ImGui::CalcTextSize((s + "...").c_str()).x > width) s.pop_back();
        s += "...";
    }

    ImGui::PopFont();
    dl->AddText(font, size, at, col, s.c_str());
}

void monoLines(App& app, const std::vector<std::string>& lines, bool& follow) {
    ImGui::PushFont(app.fonts.mono, 12.5f);
    ImGuiListClipper clip;
    clip.Begin(int(lines.size()));

    while (clip.Step()) {
        for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
            ImGui::TextUnformatted(lines[size_t(k)].c_str());
        }
    }

    // Follows the end unless scrolled up from it.
    if (follow) ImGui::SetScrollHereY(1.0f);
    follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
    ImGui::PopFont();
}

// The step a build is on, from the banners new_instance.sh and the builders
// print.
std::string buildStep(const std::vector<std::string>& lines) {
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const std::string& l = *it;

        if (l.rfind("=== ", 0) == 0 || l.rfind("--- ", 0) == 0 || l.rfind("### ", 0) == 0) {
            std::string s = l.substr(4);
            while (!s.empty() && (s.back() == '-' || s.back() == '=' || s.back() == ' ')) s.pop_back();
            return s;
        }
    }

    return lines.empty() ? "Starting" : lines.back();
}

void startInstance(App& app, const Instance& i) {
    std::string error;

    if (!app.machines->start(i, error)) showMessage(app, error);
}

void rebuild(App& app, const Instance& i) {
    BuildRequest r;
    r.name = i.name;
    r.firmware = i.firmware;
    r.product = i.product;
    r.montero = i.montero;
    r.force = true;
    std::string error;

    if (!app.machines->build(r, error)) showMessage(app, error);
}

// Start, stop, edit and the rest, for the row's context menu and the
// toolbar's More menu alike.
void instanceMenu(App& app, const Instance& i) {
    const bool stopped = i.state == MachineState::Stopped;

    if (stopped && ImGui::MenuItem("Start")) startInstance(app, i);
    if (i.state == MachineState::Running && ImGui::MenuItem("Stop")) app.machines->stop(i);
    if (i.state == MachineState::Stopping && ImGui::MenuItem("Force Stop")) app.machines->stop(i, true);
    if (i.state == MachineState::Building && ImGui::MenuItem("Cancel Build")) app.machines->cancelBuild(i);

    ImGui::Separator();
    const bool idle = stopped || i.state == MachineState::Failed;

    if (ImGui::MenuItem("Edit...", nullptr, false, idle)) wizardOpen(app, &i);
    if (ImGui::MenuItem("Rebuild", nullptr, false, idle && !i.firmware.empty())) rebuild(app, i);
    if (ImGui::MenuItem("Show in Finder")) openPath(i.dir);
    if (i.sshPort && ImGui::MenuItem("Copy SSH Command")) {
        const std::string ssh = "ssh -p " + std::to_string(i.sshPort) + " root@localhost";
        SDL_SetClipboardText(ssh.c_str());
    }

    ImGui::Separator();

    if (ImGui::MenuItem("Delete...", nullptr, false, idle)) app.confirmDelete = i.name;
}

// The circle at the end of a row: start, stop, or what it is doing.
void rowAction(App& app, const Instance& i, ImVec2 centre) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = 14.0f;

    if (i.state == MachineState::Building || i.state == MachineState::Stopping) {
        spinner(dl, centre, 8.0f, u32(kAmber));
        return;
    }

    if (i.state == MachineState::Failed) {
        drawIcon(dl, Icon::Alert, centre, 20.0f, u32(kRed));
        return;
    }

    ImGui::SetCursorScreenPos(centre - ImVec2(r, r));
    ImGui::SetNextItemAllowOverlap();
    ImGui::PushID("action");
    const bool pressed = ImGui::InvisibleButton("##go", {2 * r, 2 * r});
    const bool hot = ImGui::IsItemHovered();
    ImGui::PopID();

    const bool running = i.state == MachineState::Running;
    dl->AddCircleFilled(centre, r, hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(235, 235, 240, 235));
    drawIcon(dl, running ? Icon::Stop : Icon::Play, centre, 13.0f, IM_COL32(25, 25, 30, 255));

    if (hot) ImGui::SetTooltip(running ? "Stop" : "Start");

    if (pressed) {
        if (running) app.machines->stop(i);
        else startInstance(app, i);
        selectInstance(app, i.name);
    }
}

void sidebar(App& app, float width) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize({width, vp->WorkSize.y});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.115f, 0.105f, 0.135f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10, 14});
    ImGui::Begin("##sidebar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    // Title, and the plus that adds a device, centred on the same line as
    // the main area's toolbar, 32 down, so the sidebar toggle lines up.
    ImGui::SetCursorPos({16, 32 - 12});
    ImGui::PushFont(app.fonts.title, 24.0f);
    ImGui::TextUnformatted("QEngine");
    ImGui::PopFont();
    ImGui::SameLine(width - 46);
    ImGui::SetCursorPosY(32 - 16);

    if (iconButton("##new", Icon::Plus, "New Device (Cmd+N)")) wizardOpen(app);

    ImGui::Dummy({0, 6});

    // Layouts sit under the list, for drawing a panel without a guest.
    const float layoutsHeight = app.layoutsOpen ? std::min(46.0f * float(app.devices.size()) + 40.0f, 300.0f) : 34.0f;
    ImGui::BeginChild("##list", {0, -layoutsHeight}, 0, ImGuiWindowFlags_NoBackground);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (app.instances.empty()) {
        ImGui::Dummy({0, 10});
        ImGui::SetCursorPosX(16);
        ImGui::TextDisabled("No devices yet.");
        ImGui::SetCursorPosX(16);

        if (pillButton("New Device", Icon::Plus, true)) wizardOpen(app);
    }

    for (const Instance& i : app.instances) {
        ImGui::PushID(i.name.c_str());
        const bool sel = app.pane == Pane::Instance && app.selected == i.name;
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowW = ImGui::GetContentRegionAvail().x;
        const float rowH = 56.0f;

        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##row", {rowW, rowH})) selectInstance(app, i.name);
        const bool hot = ImGui::IsItemHovered();

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            i.state == MachineState::Stopped) {
            startInstance(app, i);
        }

        if (ImGui::BeginPopupContextItem("##menu")) {
            instanceMenu(app, i);
            ImGui::EndPopup();
        }

        const ImVec2 rowMax = rowMin + ImVec2(rowW, rowH);

        if (sel) dl->AddRectFilled(rowMin, rowMax, u32(kAccent, 0.85f), 8.0f);
        else if (hot) dl->AddRectFilled(rowMin, rowMax, IM_COL32(255, 255, 255, 14), 8.0f);

        const Product* p = findProduct(i.product);
        deviceIcon(dl, rowMin + ImVec2(8, 9), 38.0f, familyOf(i), i.product);

        // The state on the icon, as a dot, unless it is simply stopped.
        if (i.state != MachineState::Stopped) {
            const ImVec2 dot = rowMin + ImVec2(8 + 38 - 2, 9 + 38 - 2);
            dl->AddCircleFilled(dot, 6.0f, sel ? u32(kAccent) : IM_COL32(29, 27, 34, 255));
            dl->AddCircleFilled(dot, 4.0f, u32(stateColour(i)));
        }

        const float textX = 56.0f;
        const float textW = rowW - textX - 40.0f;
        const ImU32 sub = sel ? IM_COL32(255, 255, 255, 190) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
        clippedText(dl, app.fonts.ui, 15.5f, rowMin + ImVec2(textX, 9), ImGui::GetColorU32(ImGuiCol_Text), i.name,
                    textW);

        std::string line = p ? p->name : i.product.empty() ? std::string(stateName(i)) : i.product;
        const std::string v = version(i);

        if (!v.empty()) line += "  " + v;
        if (i.state == MachineState::Building || i.state == MachineState::Failed) line = stateName(i);

        clippedText(dl, app.fonts.ui, 12.5f, rowMin + ImVec2(textX, 30), sub, line, textW);

        // The start button shows on the selected row and the one under the
        // mouse, and always while something is happening.
        if (sel || hot || i.state != MachineState::Stopped) {
            rowAction(app, i, ImVec2(rowMax.x - 22, rowMin.y + rowH * 0.5f));
        }

        ImGui::SetCursorScreenPos(rowMin + ImVec2(0, rowH + 4));
        ImGui::Dummy({0, 0});
        ImGui::PopID();
    }

    ImGui::EndChild();

    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.05f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1, 1, 1, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::SetNextItemOpen(app.layoutsOpen);
    app.layoutsOpen = ImGui::CollapsingHeader("LAYOUTS");
    ImGui::PopStyleColor(4);

    if (app.layoutsOpen) {
        ImGui::BeginChild("##layouts", {0, 0}, 0, ImGuiWindowFlags_NoBackground);

        for (int k = 0; k < int(app.devices.size()); ++k) {
            const Device& d = app.devices[size_t(k)];
            std::string codes;

            for (const std::string& c : d.layout.products) codes += (codes.empty() ? "" : " ") + c;

            ImGui::PushID(k);
            const bool sel = app.pane == Pane::Layout && app.current == k;

            if (ImGui::Selectable("##layout", sel, 0, {0, 38})) {
                disconnectGuest(app);
                app.pane = Pane::Layout;
                selectDevice(app, k);
            }

            const ImVec2 min = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(app.fonts.ui, 14.0f, min + ImVec2(8, 3),
                                                ImGui::GetColorU32(ImGuiCol_Text), d.layout.name.c_str());
            ImGui::GetWindowDrawList()->AddText(app.fonts.ui, 12.0f, min + ImVec2(8, 21),
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled), codes.c_str());
            ImGui::PopID();
        }

        ImGui::EndChild();
    }

    ImGui::End();
    ImGui::PopStyleColor();
}

// A row on the welcome page, icon then label.
bool tile(const char* id, Icon icon, const char* label, ImVec2 size) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hot = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, min + size, hot ? IM_COL32(58, 58, 64, 255) : IM_COL32(46, 46, 51, 255), 10.0f);

    const ImVec2 c = min + ImVec2(size.y * 0.5f + 8, size.y * 0.5f);
    dl->AddCircle(c, 22.0f, IM_COL32(235, 235, 240, 255), 48, 2.5f);
    drawIcon(dl, icon, c, 20.0f, IM_COL32(235, 235, 240, 255));

    ImGui::PushFont(nullptr, 16.0f);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(min + ImVec2(size.y + 20, (size.y - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label);
    ImGui::PopFont();
    return pressed;
}

// Shows or hides the sidebar, from the top left of the main area. Cmd+Ctrl+S
// does the same, as it does in Finder and Xcode.
void sidebarToggle(App& app) {
    const char* tip = app.sidebarOpen ? "Hide Sidebar (Cmd+Ctrl+S)" : "Show Sidebar (Cmd+Ctrl+S)";

    if (iconButton("##sidebar", Icon::Sidebar, tip)) app.sidebarOpen = !app.sidebarOpen;
}

void settingsButton(App& app) {
    if (iconButton("##settings", Icon::Settings, "Settings")) app.settingsOpen = true;
}

void welcome(App& app) {
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetCursorScreenPos(top + ImVec2(14, 16));
    sidebarToggle(app);
    ImGui::SetCursorScreenPos(top + ImVec2(avail.x - 48, 16));
    settingsButton(app);
    ImGui::SetCursorScreenPos(top);

    const ImVec2 t(430, 76);
    const float gap = 10;
    const ImVec2 row(t.x, t.y * 3 + gap * 2);
    const ImVec2 origin = ImGui::GetCursorScreenPos() + (avail - row) * 0.5f + ImVec2(0, 20);

    ImGui::PushFont(app.fonts.ui, 26.0f);
    const char* title = "Welcome to QEngine";
    const ImVec2 ts = ImGui::CalcTextSize(title);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + (row.x - ts.x) * 0.5f, origin.y - 50));
    ImGui::TextUnformatted(title);
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(origin);
    if (tile("##new", Icon::Plus, "Create a New Device", t)) wizardOpen(app);
    ImGui::SetCursorScreenPos(origin + ImVec2(0, t.y + gap));
    if (tile("##guide", Icon::Book, "Instances Guide", t)) {
        openPath(app.args.repo / "scripts/build_scripts/INSTANCES.md");
    }
    ImGui::SetCursorScreenPos(origin + ImVec2(0, (t.y + gap) * 2));
    if (tile("##folder", Icon::Folder, "Instances Folder", t)) {
        std::error_code ec;
        fs::create_directories(app.args.instances, ec);
        openPath(app.args.instances);
    }
}

// The View popover: drawn or screen alone, and the panel's own options.
void viewMenu(App& app) {
    int view = int(app.options.view);
    ImGui::TextDisabled("VIEW");
    ImGui::RadioButton("Drawn", &view, int(View::Drawn));
    ImGui::RadioButton("Screen in bezel", &view, int(View::Screen));
    app.options.view = View(view);

    ImGui::Separator();
    ImGui::BeginDisabled(app.link.connected());
    ImGui::Checkbox("Lamps from layout", &app.options.layoutLamps);
    ImGui::EndDisabled();
    ImGui::Checkbox("Animate meters", &app.options.animateMeters);
    ImGui::Checkbox("Show control ids", &app.options.showIds);
}

// The bar across the top of the main area: what is selected, and what can be
// done to it.
void toolbar(App& app, const Instance* inst) {
    const float h = 64.0f;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, min + ImVec2(w, h), IM_COL32(32, 31, 36, 255));
    dl->AddLine(min + ImVec2(0, h - 1), min + ImVec2(w, h - 1), IM_COL32(0, 0, 0, 120));

    std::string title;
    std::string sub;
    ImVec4 subColour = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);

    if (inst) {
        title = inst->name;
        sub = stateName(*inst);
        subColour = stateColour(*inst);
    } else if (app.current >= 0) {
        const Device& d = app.devices[size_t(app.current)];
        title = d.layout.name;
        sub = d.path.filename().string() + ", reloads on save";
    }

    ImGui::SetCursorScreenPos(min + ImVec2(14, (h - 32) * 0.5f));
    sidebarToggle(app);

    const float textX = 58.0f;
    dl->AddText(app.fonts.ui, 18.0f, min + ImVec2(textX, 12), ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
    dl->AddText(app.fonts.ui, 13.0f, min + ImVec2(textX, 35), u32(subColour), sub.c_str());

    if (inst) {
        const float subW = app.fonts.ui->CalcTextSizeA(13.0f, FLT_MAX, 0, sub.c_str()).x;
        std::string detail = productName(inst->product);
        const std::string v = version(*inst);

        if (!v.empty()) detail += "  " + v;
        if (!inst->arch.empty()) detail += "  " + std::string(inst->arch == "armhf" ? "armv7" : inst->arch);

        dl->AddText(app.fonts.ui, 13.0f, min + ImVec2(textX + subW + 10, 35),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), detail.c_str());
    }

    // Buttons from the right.
    float x = min.x + w - 16;
    auto place = [&](float width) {
        x -= width;
        ImGui::SetCursorScreenPos(ImVec2(x, min.y + (h - 32) * 0.5f));
        x -= 8;
    };

    place(32);
    settingsButton(app);

    if (inst) {
        place(32);

        if (iconButton("##more", Icon::More, "More")) ImGui::OpenPopup("##moremenu");

        if (ImGui::BeginPopup("##moremenu")) {
            instanceMenu(app, *inst);
            ImGui::EndPopup();
        }

        place(32);

        if (iconButton("##console", Icon::Console, "Console", app.console)) app.console = !app.console;
    }

    if (app.current >= 0) {
        place(32);

        if (iconButton("##view", Icon::Eye, "View")) ImGui::OpenPopup("##viewmenu");

        if (ImGui::BeginPopup("##viewmenu")) {
            viewMenu(app);
            ImGui::EndPopup();
        }
    }

    if (inst) {
        // The main action, as text, since it changes.
        const char* label = nullptr;
        Icon icon = Icon::Play;
        bool primary = true;
        bool danger = false;

        switch (inst->state) {
        case MachineState::Stopped: label = "Start"; break;
        case MachineState::Running: label = "Stop"; icon = Icon::Stop; primary = false; break;
        case MachineState::Stopping: label = "Force Stop"; icon = Icon::Stop; primary = false; danger = true; break;
        case MachineState::Building: label = "Cancel Build"; icon = Icon::Close; primary = false; break;
        case MachineState::Failed: label = "Retry"; icon = Icon::Refresh; break;
        }

        const float lw = ImGui::CalcTextSize(label).x + 50;
        place(lw);

        if (pillButton(label, icon, primary, danger)) {
            switch (inst->state) {
            case MachineState::Stopped: startInstance(app, *inst); break;
            case MachineState::Running: app.machines->stop(*inst); break;
            case MachineState::Stopping: app.machines->stop(*inst, true); break;
            case MachineState::Building: app.machines->cancelBuild(*inst); break;
            case MachineState::Failed: rebuild(app, *inst); break;
            }
        }
    }

    ImGui::SetCursorScreenPos(min + ImVec2(0, h));
}

// The guest's screen on its own, for a product with no panel layout.
void screenOnly(App& app, const Instance& inst) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(pos, pos + avail, IM_COL32(34, 36, 41, 255), IM_COL32(34, 36, 41, 255),
                                IM_COL32(14, 15, 17, 255), IM_COL32(14, 15, 17, 255));

    if (avail.x < 50 || avail.y < 50) return;

    const bool live = app.screen.connected() && app.screen.texture() != ImTextureID_Invalid;

    if (!live) {
        // Stopped, the start button takes the middle, so only the note shows.
        const float s = 96.0f;
        const bool stopped = inst.state == MachineState::Stopped;
        const ImVec2 c = pos + avail * 0.5f + ImVec2(0, stopped ? 60.0f : 0.0f);

        if (!stopped) deviceIcon(dl, c - ImVec2(s * 0.5f, s * 0.5f + 40), s, familyOf(inst), inst.product);

        const std::string name = productName(inst.product);
        const std::string note = inst.state == MachineState::Running
                                     ? "Waiting for the screen on :" + std::to_string(app.screen.port())
                                     : "No panel layout for " + inst.product + ". Its screen shows here while it runs.";
        ImVec2 ts = app.fonts.ui->CalcTextSizeA(18.0f, FLT_MAX, 0, name.c_str());
        if (!stopped) {
            dl->AddText(app.fonts.ui, 18.0f, c + ImVec2(-ts.x * 0.5f, s * 0.5f - 24),
                        ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
        }

        ts = app.fonts.ui->CalcTextSizeA(13.5f, FLT_MAX, 0, note.c_str());
        dl->AddText(app.fonts.ui, 13.5f, c + ImVec2(-ts.x * 0.5f, s * 0.5f + 4),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), note.c_str());
        ImGui::Dummy(avail);
        return;
    }

    const ImVec2 size = app.screen.size();
    const float margin = std::min(avail.x, avail.y) * 0.04f;
    const float scale = std::min((avail.x - 2 * margin) / size.x, (avail.y - 2 * margin) / size.y);
    const ImVec2 min = pos + (avail - size * scale) * 0.5f;
    const ImVec2 max = min + size * scale;

    ImGui::InvisibleButton("##screen", avail);
    dl->AddRectFilled(min - ImVec2(10, 10), max + ImVec2(10, 10), IM_COL32(8, 8, 9, 255), 10.0f);
    dl->AddImage(app.screen.texture(), min, max);

    // A press, held while dragged and let go wherever the mouse is then.
    if (ImGui::IsItemActive() || app.screenHeld) {
        const ImVec2 uv = ImClamp((ImGui::GetIO().MousePos - min) / ImMax(max - min, ImVec2(1, 1)), ImVec2(0, 0),
                                  ImVec2(1, 1));
        const bool down = ImGui::IsItemActive();
        app.screen.pointer(uv, down);
        app.screenHeld = down;
    }
}

// The guest's screen alone, in a frame coloured as the device is, from the
// layout's "bezel": an MPC One's red, or a Prime 4 G2's champagne strip over
// its black deck. Without a guest, as a layout on its own, the screen is
// blank at the lcd's shape.
void bezelScreen(App& app, const Instance* inst, const Layout& layout) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(pos, pos + avail, IM_COL32(34, 36, 41, 255), IM_COL32(34, 36, 41, 255),
                                IM_COL32(14, 15, 17, 255), IM_COL32(14, 15, 17, 255));

    if (avail.x < 50 || avail.y < 50) return;

    const nlohmann::json& bezel = layout.bezel;
    const nlohmann::json& body = layout.faces.empty() ? bezel : layout.faces[0].body;
    const ImVec4 bodyColour = flag(body, "none") ? ImVec4(0.08f, 0.08f, 0.09f, 1)
                                                 : colour(body, "color", ImVec4(0.08f, 0.08f, 0.09f, 1));

    struct Band {
        ImU32 col;
        float share;
    };
    std::vector<Band> bands;
    float total = 0;

    for (const nlohmann::json& b : bezel.value("bands", nlohmann::json::array())) {
        if (!b.is_array() || b.empty() || !b[0].is_string()) continue;

        const float share = b.size() > 1 && b[1].is_number() ? std::max(b[1].get<float>(), 0.0f) : 1.0f;
        bands.push_back({u32(parseColour(b[0].get<std::string>(), bodyColour)), share});
        total += share;
    }

    if (bands.empty() || total <= 0) {
        bands = {{u32(bodyColour), 1.0f}};
        total = 1.0f;
    }

    // The screen's own size while it is live, else the lcd's shape.
    const bool live = inst && app.screen.connected() && app.screen.texture() != ImTextureID_Invalid;
    ImVec2 size = {1280, 800};

    if (live) {
        size = app.screen.size();
    } else {
        for (const Element& e : layout.elements) {
            const ImVec4 r = rect(e.p, "rect");

            if (e.kind == "lcd" && r.z > 0 && r.w > 0) {
                size = {800.0f * r.z / r.w, 800.0f};
                break;
            }
        }
    }

    // Everything in the screen's pixels, then scaled to fit.
    const float border = num(bezel, "border", 0.09f) * std::min(size.x, size.y);
    const ImVec2 outer = size + ImVec2(border, border) * 2;
    const float margin = std::min(avail.x, avail.y) * 0.04f;
    const float scale = std::min((avail.x - 2 * margin) / outer.x, (avail.y - 2 * margin) / outer.y);
    const ImVec2 omin = pos + (avail - outer * scale) * 0.5f;
    const ImVec2 omax = omin + outer * scale;
    const ImVec2 min = omin + ImVec2(border, border) * scale;
    const ImVec2 max = min + size * scale;
    const float radius = border * scale * 0.5f;

    ImGui::InvisibleButton("##screen", avail);

    dl->AddRectFilled(omin + ImVec2(0, 6), omax + ImVec2(0, 10), IM_COL32(0, 0, 0, 90), radius);
    float y = omin.y;

    for (size_t i = 0; i < bands.size(); ++i) {
        const float h = (omax.y - omin.y) * bands[i].share / total;
        const bool first = i == 0;
        const bool last = i + 1 == bands.size();
        const ImDrawFlags corners = first && last ? ImDrawFlags_RoundCornersAll
                                    : first       ? ImDrawFlags_RoundCornersTop
                                    : last        ? ImDrawFlags_RoundCornersBottom
                                                  : ImDrawFlags_RoundCornersNone;
        dl->AddRectFilled(ImVec2(omin.x, y), ImVec2(omax.x, last ? omax.y : y + h), bands[i].col, radius, corners);

        if (!first) dl->AddLine(ImVec2(omin.x, y), ImVec2(omax.x, y), IM_COL32(0, 0, 0, 70), 1.0f);

        y += h;
    }

    const ImU32 edge = hasColour(bezel, "edge")  ? u32(colour(bezel, "edge", {}))
                       : hasColour(body, "edge") ? u32(colour(body, "edge", {}))
                                                 : IM_COL32(255, 255, 255, 30);
    dl->AddRect(omin, omax, edge, radius, 0, 1.5f);

    // A black lip round the glass, as every one of them has.
    const float lip = std::max(border * scale * 0.12f, 3.0f);
    dl->AddRectFilled(min - ImVec2(lip, lip), max + ImVec2(lip, lip), IM_COL32(6, 6, 7, 255), lip);

    if (live) {
        dl->AddImage(app.screen.texture(), min, max);
    } else {
        dl->AddRectFilled(min, max, IM_COL32(10, 10, 12, 255));

        if (inst && inst->state != MachineState::Stopped) {
            const std::string note = "Waiting for the screen on :" + std::to_string(app.screen.port());
            const ImVec2 ts = app.fonts.ui->CalcTextSizeA(13.5f, FLT_MAX, 0, note.c_str());
            dl->AddText(app.fonts.ui, 13.5f, (min + max - ts) * 0.5f, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                        note.c_str());
        }

        return;
    }

    // A press, held while dragged and let go wherever the mouse is then.
    if (ImGui::IsItemActive() || app.screenHeld) {
        const ImVec2 uv = ImClamp((ImGui::GetIO().MousePos - min) / ImMax(max - min, ImVec2(1, 1)), ImVec2(0, 0),
                                  ImVec2(1, 1));
        const bool down = ImGui::IsItemActive();
        app.screen.pointer(uv, down);
        app.screenHeld = down;
    }
}

// The big start button over a stopped device's panel.
void playOverlay(App& app, const Instance& inst, ImVec2 min, ImVec2 max) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(10, 10, 12, 110));

    const float r = 42.0f;
    const ImVec2 c = (min + max) * 0.5f;
    ImGui::SetCursorScreenPos(c - ImVec2(r, r));
    ImGui::BeginChild("##play", {2 * r, 2 * r}, 0,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    const bool pressed = ImGui::InvisibleButton("##start", {2 * r, 2 * r});
    const bool hot = ImGui::IsItemHovered();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    cdl->AddCircleFilled(c, r, hot ? IM_COL32(255, 255, 255, 250) : IM_COL32(240, 240, 244, 225), 64);
    drawIcon(cdl, Icon::Play, c, 34.0f, IM_COL32(25, 25, 30, 255));
    ImGui::EndChild();

    if (pressed) startInstance(app, inst);
}

// The strip along the bottom: the instance's settings while it is stopped,
// and how the panel's link to the guest is doing while it runs. A control
// under the mouse takes it over, for checking a layout's numbers.
void statusStrip(App& app, const Instance* inst) {
    const float h = 30.0f;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, min + ImVec2(w, h), IM_COL32(26, 25, 30, 255));
    dl->AddLine(min, min + ImVec2(w, 0), IM_COL32(0, 0, 0, 120));
    ImGui::SetCursorScreenPos(min + ImVec2(14, 7));

    auto item = [&](ImVec4 dot, const std::string& text, const std::string& tip = "") {
        const ImVec2 at = ImGui::GetCursorScreenPos();

        if (dot.w > 0) {
            dl->AddCircleFilled(at + ImVec2(4, 8), 3.5f, u32(dot));
            ImGui::Dummy({8, 0});
            ImGui::SameLine(0, 6);
        }

        ImGui::TextDisabled("%s", text.c_str());

        if (!tip.empty() && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(420);
            ImGui::TextUnformatted(tip.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }

        ImGui::SameLine(0, 22);
    };

    // The Screen view draws no panel, so what was hovered in it is stale.
    const Element* e = app.current >= 0 && app.options.view != View::Screen ? app.surface.hovered() : nullptr;
    const std::string error = app.current >= 0 ? app.devices[size_t(app.current)].error : "";

    if (!error.empty()) {
        ImGui::TextColored(kRed, "%s", error.c_str());
    } else if (e) {
        const ImVec2 at = vec2(e->p, "at");
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s at %.1f, %.1f mm", e->kind.c_str(), at.x, at.y);
        item({}, e->id);
        item({}, buf);

        if (e->p.contains("midi")) item({}, "midi " + e->p["midi"].dump());
    } else if (inst && (inst->state == MachineState::Running || inst->state == MachineState::Stopping)) {
        if (inst->port) {
            if (app.link.connected()) {
                // midibridge and mpcsurface replay the guest's lamps the
                // moment a client connects, so silence means nothing in it
                // reads the port.
                const bool quiet = app.link.received() == 0;
                item(quiet ? kAmber : kGreen,
                     "MIDI :" + std::to_string(app.link.port()) + "  " + std::to_string(app.link.sent()) + " sent, " +
                         std::to_string(app.link.received()) + " received",
                     quiet ? "Nothing from the guest yet. Engine only binds the panel when it starts, and an image "
                             "built before it served MIDI to the host has nothing reading this port; rebuild it."
                           : "");
            } else {
                item(kAmber, "MIDI waiting for :" + std::to_string(app.link.port()), app.link.problem());
            }
        }

        if (inst->screenPort) {
            const char* what = inst->engineVnc ? "Engine's VNC" : "QEMU's VNC";

            if (app.screen.connected()) {
                item(kGreen, "Screen " + app.screen.status(), std::string("From ") + what + " on :" +
                                                                   std::to_string(app.screen.port()));
            } else {
                std::string tip = app.screen.status();

                if (!inst->engineVnc && inst->display != "vnc" && inst->display != "egl-vnc") {
                    tip += "\nStarted outside QEngine with DISPLAY_MODE=" + inst->display +
                           ", which opens its own window and serves no VNC. Start it from here instead.";
                }

                item(kAmber, "Screen waiting for :" + std::to_string(app.screen.port()), tip);
            }
        }

        item({}, "SSH :" + std::to_string(inst->sshPort));
    } else if (inst && inst->ready) {
        const std::string name = productName(inst->product);
        item({}, name == inst->product ? name : name + " (" + inst->product + ")");
        item({}, inst->arch == "armhf" ? "armv7" : inst->arch);
        item({}, fs::path(inst->firmware).filename().string());

        if (inst->montero) item({}, "MONTERO");

        item({}, "SSH :" + std::to_string(inst->sshPort));
        if (inst->port) item({}, "MIDI :" + std::to_string(inst->port));
        if (inst->screenPort) item({}, "VNC :" + std::to_string(inst->screenPort));
    } else if (app.current >= 0) {
        item({}, std::to_string(app.devices[size_t(app.current)].layout.elements.size()) + " elements");
        item({}, "Hover a control");
    }

    // An item the strip's size, so the window ends exactly at its bottom.
    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy({w, h});
}

// The drawer under the panel: the VM's console, the MIDI traffic, and the
// log of the build that made it.
void consoleDrawer(App& app, const Instance& inst, float height) {
    ImGui::BeginChild("##drawer", {0, height}, 0, ImGuiWindowFlags_NoScrollbar);
    static bool follow[3] = {true, true, true};

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Console")) {
            ImGui::BeginChild("##console", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
            const auto& lines = app.consoleTail.read(app.machines->consoleLog(inst));

            if (lines.empty()) {
                ImGui::TextDisabled("The console of a VM started here shows in console.log, beside its disks.");
            }

            monoLines(app, lines, follow[0]);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (inst.port && ImGui::BeginTabItem("MIDI")) {
            ImGui::BeginChild("##midi");
            const auto& log = app.link.log();
            std::vector<std::string> lines(log.begin(), log.end());
            monoLines(app, lines, follow[1]);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Build Log")) {
            ImGui::BeginChild("##build", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
            const auto& lines = app.buildTail.read(app.machines->buildLog(inst));

            if (lines.empty()) ImGui::TextDisabled("Built outside QEngine, so there is no build log.");

            monoLines(app, lines, follow[2]);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndChild();
}

// A build in progress or one that failed: what it is doing, and its log.
void buildPane(App& app, const Instance& inst) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float w = std::min(avail.x - 48, 900.0f);
    const float x = (avail.x - w) * 0.5f;
    const auto& lines = app.buildTail.read(app.machines->buildLog(inst));
    const bool failed = inst.state == MachineState::Failed;

    ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(x, 28));
    const ImVec2 head = ImGui::GetCursorScreenPos();

    if (failed) {
        drawIcon(ImGui::GetWindowDrawList(), Icon::Alert, head + ImVec2(14, 16), 28.0f, u32(kRed));
    } else {
        spinner(ImGui::GetWindowDrawList(), head + ImVec2(14, 16), 12, u32(kAccent));
    }

    ImGui::SetCursorScreenPos(head + ImVec2(42, 0));
    ImGui::PushFont(app.fonts.ui, 21.0f);
    ImGui::Text(failed ? "%s did not build" : "Building %s", inst.name.c_str());
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(head + ImVec2(42, 30));
    ImGui::PushTextWrapPos(head.x + w);

    if (failed) {
        ImGui::TextDisabled("%s", lines.empty() ? "There is no build log." : lines.back().c_str());
    } else {
        ImGui::TextDisabled("%s", buildStep(lines).c_str());
    }

    ImGui::PopTextWrapPos();

    ImGui::SetCursorScreenPos(ImVec2(head.x, ImGui::GetCursorScreenPos().y + 14));
    const float logH = ImGui::GetContentRegionAvail().y - 28;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.06f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 10});
    ImGui::BeginChild("##buildlog", {w, logH}, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_HorizontalScrollbar);
    static bool follow = true;
    monoLines(app, lines, follow);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void instancePane(App& app, const Instance& inst) {
    toolbar(app, &inst);

    if (!inst.ready || inst.state == MachineState::Building) {
        buildPane(app, inst);
        return;
    }

    const float strip = 30.0f;
    const float drawer = app.console ? std::min(260.0f, ImGui::GetContentRegionAvail().y * 0.4f) : 0.0f;

    ImGui::BeginChild("##panel", {0, -(strip + drawer)}, 0,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 pmin = ImGui::GetCursorScreenPos();
    const ImVec2 pmax = pmin + ImGui::GetContentRegionAvail();

    if (app.current >= 0 && app.options.view == View::Screen) {
        bezelScreen(app, &inst, app.devices[size_t(app.current)].layout);

        // The panel is not drawn, but the guest's lamps still land in it.
        if (app.link.connected()) app.options.layoutLamps = true;

        app.link.update(app.surface, ImGui::GetTime());
    } else if (app.current >= 0) {
        app.options.screen = {};

        if (app.screen.connected() && app.screen.texture() != ImTextureID_Invalid) {
            app.options.screen = {app.screen.texture(), app.screen.size()};
        }

        // With a guest connected its lamps are the ones shown.
        if (app.link.connected()) app.options.layoutLamps = true;

        app.surface.draw(app.options, app.fonts, app.settings);
        app.link.update(app.surface, ImGui::GetTime());

        // Every frame, so a press dragged off the screen still lets go; only
        // a change is sent.
        const ScreenPointer& sp = app.surface.screenPointer();
        app.screen.pointer(sp.uv, sp.down);
    } else {
        screenOnly(app, inst);
    }

    if (inst.state == MachineState::Stopped) playOverlay(app, inst, pmin, pmax);

    ImGui::EndChild();
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y));

    if (app.console) {
        consoleDrawer(app, inst, drawer);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y));
    }

    statusStrip(app, &inst);
}

void layoutPane(App& app) {
    toolbar(app, nullptr);
    ImGui::BeginChild("##panel", {0, -30.0f}, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    app.options.screen = {};

    if (app.options.view == View::Screen) {
        bezelScreen(app, nullptr, app.devices[size_t(app.current)].layout);
    } else {
        app.surface.draw(app.options, app.fonts, app.settings);
    }

    ImGui::EndChild();
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y));
    statusStrip(app, nullptr);
}

void settingsModal(App& app) {
    if (!app.settingsOpen) return;

    if (!ImGui::IsPopupOpen("Settings")) ImGui::OpenPopup("Settings");

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({std::min(400.0f, vp->WorkSize.x - 32), 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.15f, 0.15f, 0.17f, 1));
    const bool shown = ImGui::BeginPopupModal("Settings", &app.settingsOpen,
                                              ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    if (shown) {
        ImGui::TextUnformatted("Jogwheel sensitivity");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##jogSensitivity", &app.settings.jogSensitivity, Settings::minJogSensitivity,
                               Settings::maxJogSensitivity, "%.2fx",
                               ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
            app.settingsDirty = true;
        }

#ifdef __APPLE__
        if (ImGui::Checkbox("Enable haptics", &app.settings.haptics)) app.settingsDirty = true;
#endif

        ImGui::Separator();
        if (pillButton("Done", Icon::Check, true) || ImGui::Shortcut(ImGuiKey_Escape)) {
            app.settingsOpen = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (!app.settingsOpen) saveSettings(app);
}

void deleteModal(App& app) {
    if (!app.confirmDelete.empty() && !ImGui::IsPopupOpen("Delete Device")) ImGui::OpenPopup("Delete Device");

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({420, 0});

    if (!ImGui::BeginPopupModal("Delete Device", nullptr,
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }

    ImGui::PushFont(app.fonts.ui, 18.0f);
    ImGui::Text("Delete %s?", app.confirmDelete.c_str());
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Its rootfs and its /data disk are removed with it. The firmware image is kept.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0, 8});

    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 200);

    if (pillButton("Cancel", Icon::Close, false) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        app.confirmDelete.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();

    if (pillButton("Delete", Icon::Delete, false, true)) {
        for (const Instance& i : app.instances) {
            if (i.name != app.confirmDelete) continue;

            std::string error;

            if (app.machines->remove(i, error)) {
                if (app.selected == i.name) {
                    disconnectGuest(app);
                    app.pane = Pane::Welcome;
                    app.selected.clear();
                }
            } else {
                showMessage(app, error);
            }
        }

        app.confirmDelete.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void messageToast(App& app) {
    if (app.message.empty() || ImGui::GetTime() - app.messageAt > 6.0) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos + vp->WorkSize - ImVec2(20, 50), ImGuiCond_Always, {1, 1});
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PushTextWrapPos(380);
    ImGui::TextColored(kRed, "%s", app.message.c_str());
    ImGui::PopTextWrapPos();
    ImGui::End();
}

// Connects the panel to the selected instance while it runs, and lets go of
// it otherwise. One guest at a time, since there is one panel.
void syncGuest(App& app) {
    const Instance* inst = app.pane == Pane::Instance ? selectedInstance(app) : nullptr;
    const bool live = inst && inst->ready &&
                      (inst->state == MachineState::Running || inst->state == MachineState::Stopping);
    const std::string want = live ? inst->name : "";

    if (want == app.linked) return;

    disconnectGuest(app);
    app.linked = want;

    if (!live) return;

    if (inst->port > 0) app.link.connect(inst->port);
    if (inst->screenPort > 0) app.screen.connect(inst->screenPort);
}

}  // namespace

void disconnectGuest(App& app) {
    app.link.disconnect();
    app.screen.disconnect();
    app.linked.clear();
    app.screenHeld = false;
}

std::string productName(const std::string& code) {
    const Product* p = findProduct(code);
    return p ? p->name : code.empty() ? "Unknown product" : code;
}

std::string familyOf(const Instance& i) {
    if (const Product* p = findProduct(i.product)) return p->family;
    if (i.device == "engine") return "Engine";
    if (i.device == "mpc" || i.device == "mpc-sample") return "MPC";
    if (i.device == "headrush") return "HeadRush";
    if (i.device == "drummodule") return "Alesis";
    return "";
}

// An instance's icon is coloured by the OS it runs: Engine's green, the red
// of the MPC logo, and placeholders for the rest until they get their own.
ImU32 familyColour(const std::string& family, float alpha) {
    ImVec4 c = {0.42f, 0.42f, 0.47f, 1};  // unknown

    if (family == "Engine") c = {0.16f, 0.74f, 0.42f, 1};
    else if (family == "MPC") c = {0.84f, 0.11f, 0.15f, 1};
    else if (family == "HeadRush") c = {0.45f, 0.40f, 0.58f, 1};  // placeholder
    else if (family == "Alesis") c = {0.36f, 0.48f, 0.60f, 1};    // placeholder

    return u32(c, alpha);
}

namespace {
ImFont* gIcons = nullptr;
}

void setIconFont(ImFont* font) { gIcons = font; }

// The glyph about `s` across, centred on `c`.
void drawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float s, ImU32 col) {
    if (!gIcons) return;

    const float size = s * 1.25f;
    const ImWchar cp = ImWchar(icon);
    ImFontBaked* baked = gIcons->GetFontBaked(size);

    char utf8[5] = {};
    ImTextCharToUtf8(utf8, cp);
    // Each icon is drawn centred in a square the size of the em, which in
    // MDI's font runs from 64 units below the baseline to 448 above it, of
    // 512. So its middle is half the advance across, and 0.375 of the size
    // above the baseline. ImGui's own outline of a glyph is no use here: it
    // puts the left edge at the pen and pads the top and bottom unevenly,
    // which threw icons a few points right and up or down.
    const ImVec2 mid(size * 0.5f, baked->Ascent - size * 0.375f);
    dl->AddText(gIcons, size, ImFloor(c - mid), col, utf8);
}

// HeadRush is three kinds of device under one name: guitar processors, the
// VX5 vocal processor and the loopers, each with its own icon.
Icon productIcon(const std::string& family, const std::string& code) {
    if (family == "Engine") return Icon::Engine;
    if (family == "MPC") return Icon::Mpc;
    if (family == "Alesis") return Icon::Drums;

    if (family == "HeadRush") {
        if (code == "HV03") return Icon::Vocal;
        if (code == "HG08" || code == "HG03") return Icon::Looper;
        return Icon::Guitar;
    }

    return Icon::Unknown;
}

void deviceIcon(ImDrawList* dl, ImVec2 min, float s, const std::string& family, const std::string& code) {
    const ImVec2 max = min + ImVec2(s, s);
    dl->AddRectFilled(min, max, familyColour(family), s * 0.24f);
    dl->AddRectFilled(min, ImVec2(max.x, min.y + s * 0.5f), IM_COL32(255, 255, 255, 28), s * 0.24f,
                      ImDrawFlags_RoundCornersTop);
    drawIcon(dl, productIcon(family, code), min + ImVec2(s, s) * 0.5f, s * 0.5f, IM_COL32(255, 255, 255, 245));
}

bool iconButton(const char* id, Icon icon, const char* tip, bool on, float size) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, {size, size});
    const bool hot = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (on || hot || ImGui::IsItemActive()) {
        dl->AddRectFilled(min, min + ImVec2(size, size), on ? u32(kAccent, 0.5f) : IM_COL32(255, 255, 255, 24),
                          size * 0.25f);
    }

    drawIcon(dl, icon, min + ImVec2(size, size) * 0.5f, size * 0.55f, ImGui::GetColorU32(ImGuiCol_Text));

    if (hot && tip) ImGui::SetTooltip("%s", tip);

    return pressed;
}

bool pillButton(const char* label, Icon icon, bool primary, bool danger) {
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 size(ts.x + 50, 32);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hot = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec4 bg = primary ? kAccent : danger ? ImVec4(0.75f, 0.24f, 0.22f, 1) : ImVec4(0.22f, 0.22f, 0.26f, 1);

    if (hot) bg = ImVec4(bg.x * 1.12f + 0.03f, bg.y * 1.12f + 0.03f, bg.z * 1.12f + 0.03f, 1);

    dl->AddRectFilled(min, min + size, u32(bg), 8.0f);
    drawIcon(dl, icon, min + ImVec2(20, size.y * 0.5f), 13.0f, IM_COL32(255, 255, 255, 240));
    dl->AddText(min + ImVec2(34, (size.y - ts.y) * 0.5f), IM_COL32(255, 255, 255, 245), label);
    return pressed;
}

void spinner(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    const float a = float(ImGui::GetTime()) * 6.0f;
    dl->PathArcTo(c, r, a, a + 4.2f, 24);
    dl->PathStroke(col, 0, std::max(2.0f, r * 0.22f));
}

int layoutFor(const App& app, const std::string& product) {
    // The variant made for the product over the base that lists it.
    int found = -1;

    for (int k = 0; k < int(app.devices.size()); ++k) {
        const Device& d = app.devices[size_t(k)];
        const auto& codes = d.layout.products;

        if (std::find(codes.begin(), codes.end(), product) == codes.end()) continue;
        if (d.variant == product) return k;
        if (found < 0) found = k;
    }

    return found;
}

const Instance* selectedInstance(const App& app) {
    for (const Instance& i : app.instances) {
        if (i.name == app.selected) return &i;
    }

    return nullptr;
}

void selectInstance(App& app, const std::string& name) {
    app.pane = Pane::Instance;
    app.selected = name;

    for (const Instance& i : app.instances) {
        if (i.name == name) {
            const int d = layoutFor(app, i.product);

            if (d != app.current) selectDevice(app, d);
        }
    }
}

void showMessage(App& app, const std::string& text) {
    app.message = text;
    app.messageAt = ImGui::GetTime();
    std::fprintf(stderr, "%s\n", text.c_str());
}

void saveSettings(App& app) {
    if (!app.settingsDirty) return;

    if (app.settings.save(app.settingsFile)) {
        app.settingsDirty = false;
    } else {
        showMessage(app, "Could not save settings.");
    }
}

void drawApp(App& app) {
    app.instances = app.machines->list();

    if (!app.settingsOpen) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) wizardOpen(app);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Super | ImGuiKey_S)) app.sidebarOpen = !app.sidebarOpen;
    }

    // The selected instance can disappear, deleted from a terminal.
    if (app.pane == Pane::Instance && !selectedInstance(app)) {
        app.pane = Pane::Welcome;
        app.selected.clear();
    }

    syncGuest(app);

    const float side = app.sidebarOpen ? 290.0f : 0.0f;
    if (app.sidebarOpen) sidebar(app, side);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos + ImVec2(side, 0));
    ImGui::SetNextWindowSize(vp->WorkSize - ImVec2(side, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.13f, 0.13f, 0.145f, 1));
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();

    switch (app.pane) {
    case Pane::Welcome: welcome(app); break;
    case Pane::Instance: instancePane(app, *selectedInstance(app)); break;
    case Pane::Layout: layoutPane(app); break;
    }

    ImGui::End();
    ImGui::PopStyleVar();

    settingsModal(app);
    wizardDraw(app);
    deleteModal(app);
    messageToast(app);
}
