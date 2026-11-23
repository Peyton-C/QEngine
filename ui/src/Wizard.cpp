// The New Device sheet: pick a product, an update image and a name, and
// new_instance.sh builds it, the way a VM manager's new machine assistant
// does. Opened on a built instance it is the Edit sheet, which rebuilds that
// instance's rootfs with --force and keeps its /data.

#include "App.hpp"

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace {

const char* kSteps[] = {"Device", "Firmware", "Options", "Summary"};
constexpr ImVec4 kAccent = {0.30f, 0.56f, 1.00f, 1};
constexpr ImVec4 kRed = {1.00f, 0.45f, 0.40f, 1};

bool nameValid(const std::string& n) {
    if (n.empty() || n[0] == '.' || n[0] == '-') return false;

    for (char c : n) {
        if (!std::isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
    }

    return true;
}

std::string nameProblem(const App& app) {
    const Wizard& w = app.wizard;

    if (w.edit) return "";
    if (w.name.empty()) return "Give it a name.";
    if (!nameValid(w.name)) return "Letters, digits, dots, dashes and underscores only.";

    std::error_code ec;

    if (fs::exists(app.args.instances / w.name, ec)) return "An instance of that name exists.";

    return "";
}

void followName(App& app) {
    Wizard& w = app.wizard;

    if (!w.edit && !w.nameEdited && !w.product.empty()) {
        w.name = defaultInstanceName(w.product, fs::path(w.firmware).filename().string());
    }
}

void browse(App& app) {
    static const SDL_DialogFileFilter filters[] = {{"Firmware update", "img;zip;exe"}, {"All files", "*"}};
    SDL_ShowOpenFileDialog(
        [](void* user, const char* const* files, int) {
            auto* w = static_cast<Wizard*>(user);

            // Null for an error, and an empty list when cancelled.
            if (files && files[0]) {
                std::lock_guard<std::mutex> lock(w->browseMutex);
                w->browsed = files[0];
                w->browsedReady = true;
            }
        },
        &app.wizard, app.window, filters, 2, nullptr, false);
}

// Header and step markers.
void header(App& app) {
    Wizard& w = app.wizard;
    ImGui::PushFont(app.fonts.ui, 21.0f);
    ImGui::TextUnformatted(w.edit ? ("Edit " + w.name).c_str() : "New Device");
    ImGui::PopFont();

    float x = ImGui::GetWindowWidth() - 24;

    for (int k = 3; k >= 0; --k) {
        const float tw = ImGui::CalcTextSize(kSteps[k]).x;
        x -= tw + 34;
        const ImVec2 at = ImGui::GetWindowPos() + ImVec2(x, 24);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool done = k < w.step;
        const bool now = k == w.step;
        const ImU32 col = now || done ? ImGui::GetColorU32(kAccent) : IM_COL32(80, 80, 88, 255);
        dl->AddCircleFilled(at + ImVec2(9, 9), 9, col);
        const std::string n = std::to_string(k + 1);
        dl->AddText(app.fonts.title, 12.0f, at + ImVec2(5.5f, 2.5f), IM_COL32(255, 255, 255, 255), n.c_str());
        dl->AddText(at + ImVec2(24, 0),
                    now ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled), kSteps[k]);
    }

    ImGui::Dummy({0, 6});
    ImGui::Separator();
}

void deviceStep(App& app) {
    Wizard& w = app.wizard;
    const float listW = 340;

    ImGui::BeginChild("##products", {listW, 0}, ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "Search products and codes", w.search, sizeof w.search);

    std::string query = w.search;

    for (char& c : query) c = char(std::tolower((unsigned char)c));

    std::string family;

    for (const Product& p : catalog()) {
        std::string hay = p.name + " " + p.code + " " + p.family;

        for (char& c : hay) c = char(std::tolower((unsigned char)c));

        if (!query.empty() && hay.find(query) == std::string::npos) continue;

        if (p.family != family) {
            family = p.family;
            ImGui::Dummy({0, 4});
            std::string head = family;

            for (char& c : head) c = char(std::toupper((unsigned char)c));

            ImGui::TextDisabled("%s", head.c_str());
        }

        ImGui::PushID(p.code.c_str());

        if (ImGui::Selectable("##p", w.product == p.code, 0, {0, 38})) {
            w.product = p.code;
            w.montero = w.montero && p.montero;
            followName(app);
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) w.step = 1;

        const ImVec2 min = ImGui::GetItemRectMin();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        deviceIcon(dl, min + ImVec2(4, 5), 28, p.family, p.code);
        dl->AddText(app.fonts.ui, 14.5f, min + ImVec2(42, 2), ImGui::GetColorU32(ImGuiCol_Text), p.name.c_str());
        const std::string sub = p.code + "  " + p.arch;
        dl->AddText(app.fonts.ui, 12.0f, min + ImVec2(42, 20), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::SameLine();

    // The product's panel, drawn from its layout where there is one.
    ImGui::BeginChild("##preview", {0, 0}, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const Product* p = findProduct(w.product);

    if (!p) {
        ImGui::SetCursorPos(ImGui::GetContentRegionAvail() * 0.5f - ImVec2(90, 10));
        ImGui::TextDisabled("Choose a product to emulate.");
        ImGui::EndChild();
        return;
    }

    const int d = layoutFor(app, p->code);

    if (d != w.previewDevice) {
        w.previewDevice = d;
        w.preview.setLayout(d >= 0 ? &app.devices[size_t(d)].layout : nullptr);
    }

    const float infoH = 96;
    ImGui::BeginChild("##panel", {0, -infoH}, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (d >= 0) {
        SurfaceOptions o;
        o.view = View::Drawn;
        w.preview.draw(o, app.fonts, app.settings);
    } else {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 at = ImGui::GetCursorScreenPos() + avail * 0.5f - ImVec2(48, 48);
        deviceIcon(ImGui::GetWindowDrawList(), at, 96, p->family, p->code);
    }

    ImGui::EndChild();

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
    ImGui::PushFont(app.fonts.ui, 19.0f);
    ImGui::TextUnformatted(p->name.c_str());
    ImGui::PopFont();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
    ImGui::TextDisabled("%s, %s, %s", p->code.c_str(), p->arch.c_str(), p->family.c_str());
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
    ImGui::TextDisabled("%s", d >= 0 ? "Played from its drawn front panel, with the screen in it."
                                     : "No panel layout yet, so only its screen is shown.");
    ImGui::EndChild();
}

void firmwareStep(App& app) {
    Wizard& w = app.wizard;
    const Product* p = findProduct(w.product);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 card(std::min(avail.x, 560.0f), 190);
    ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2((avail.x - card.x) * 0.5f, (avail.y - card.y) * 0.4f));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 0.035f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24, 22});
    ImGui::BeginChild("##firmware", card, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);

    if (w.firmware.empty()) {
        ImGui::PushFont(app.fonts.ui, 18.0f);
        ImGui::TextUnformatted("Choose an update image");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("The firmware for the %s: an update .img, or the desktop updater that carries one, "
                            "the zipped Mac app or the Windows .exe.",
                            p ? p->name.c_str() : w.product.c_str());
        ImGui::PopTextWrapPos();
    } else {
        const std::string file = fs::path(w.firmware).filename().string();
        const std::string v = firmwareVersion(file);
        ImGui::PushFont(app.fonts.ui, 18.0f);
        ImGui::TextUnformatted(file.c_str());
        ImGui::PopFont();
        ImGui::TextDisabled("%s", v.empty() ? "No release in its name" : ("Release " + v).c_str());
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", fs::path(w.firmware).parent_path().string().c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::SetCursorPosY(card.y - 22 - 32);

    if (pillButton(w.firmware.empty() ? "Open..." : "Choose Another...", Icon::FolderOpen, w.firmware.empty())) {
        browse(app);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void optionsStep(App& app) {
    Wizard& w = app.wizard;
    const Product* p = findProduct(w.product);
    ImGui::Dummy({0, 6});
    ImGui::TextUnformatted("Name");
    ImGui::TextDisabled("The instance's directory under build/instances.");

    char buf[128];
    std::snprintf(buf, sizeof buf, "%s", w.name.c_str());
    ImGui::SetNextItemWidth(360);
    ImGui::BeginDisabled(w.edit);

    if (ImGui::InputText("##name", buf, sizeof buf)) {
        w.name = buf;
        w.nameEdited = true;
    }

    ImGui::EndDisabled();
    const std::string problem = nameProblem(app);

    if (!problem.empty()) ImGui::TextColored(kRed, "%s", problem.c_str());

    ImGui::Dummy({0, 14});

    if (p && p->montero) {
        ImGui::Checkbox("MONTERO", &w.montero);
        ImGui::Indent(30);
        ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 40);
        ImGui::TextDisabled("Builds the MONTERO mod loader and every mod in shims/montero/mods into the rootfs, and "
                            "preloads it into Engine.");
        ImGui::PopTextWrapPos();
        ImGui::Unindent(30);
        ImGui::Dummy({0, 14});
    }

    if (w.product == "JP22") {
        ImGui::TextDisabled("JP22 drives three screens, so GPU_MAX_OUTPUTS=3 is added to its instance.env.");
    }

    if (w.edit) {
        ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 40);
        ImGui::TextDisabled("Saving rebuilds the rootfs with --force. Its /data disk is kept, so settings and "
                            "library in the guest survive.");
        ImGui::PopTextWrapPos();
    }
}

BuildRequest request(const App& app) {
    const Wizard& w = app.wizard;
    BuildRequest r;
    r.name = w.name;
    r.firmware = w.firmware;
    r.product = w.product;
    r.montero = w.montero;
    r.force = w.edit;

    if (w.product == "JP22") r.extraEnv.push_back("GPU_MAX_OUTPUTS=3");

    return r;
}

void summaryStep(App& app) {
    Wizard& w = app.wizard;
    ImGui::Dummy({0, 6});

    if (ImGui::BeginTable("##summary", 2, ImGuiTableFlags_SizingFixedFit)) {
        auto row = [](const char* k, const std::string& v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(v.c_str());
        };

        const Product* p = findProduct(w.product);
        row("Product", (p ? p->name : w.product) + " (" + w.product + ")");
        row("Architecture", p ? p->arch : "from the firmware");
        row("Firmware", fs::path(w.firmware).filename().string());
        row("Release", firmwareVersion(fs::path(w.firmware).filename().string()));
        row("Name", w.name);
        row("MONTERO", w.montero ? "Yes" : "No");
        row("Directory", (app.args.instances / w.name).string());
        ImGui::EndTable();
    }

    ImGui::Dummy({0, 12});
    ImGui::TextDisabled("QEngine runs");
    std::string cmd = app.machines->command(request(app));
    ImGui::PushFont(app.fonts.mono, 13.0f);
    ImGui::InputTextMultiline("##cmd", cmd.data(), cmd.size() + 1, {-1, 64}, ImGuiInputTextFlags_ReadOnly);
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Building extracts the firmware and assembles the rootfs in Docker, which takes a few "
                        "minutes. It carries on if QEngine is closed.");
    ImGui::PopTextWrapPos();
}

bool canContinue(const App& app) {
    const Wizard& w = app.wizard;

    switch (w.step) {
    case 0: return findProduct(w.product) != nullptr;
    case 1: return !w.firmware.empty();
    case 2: return nameProblem(app).empty();
    default: return true;
    }
}

}  // namespace

void wizardOpen(App& app, const Instance* edit) {
    Wizard& w = app.wizard;
    w.open = true;
    w.edit = edit != nullptr;
    w.step = 0;
    w.error.clear();
    w.search[0] = 0;
    w.nameEdited = false;
    w.product.clear();
    w.firmware.clear();
    w.name.clear();
    w.montero = false;

    if (edit) {
        w.product = edit->product;
        w.name = edit->name;
        w.montero = edit->montero;
        // Recorded relative to the repo when it was given that way.
        const fs::path f = edit->firmware;
        w.firmware = f.empty() ? "" : (f.is_absolute() ? f : app.args.repo / f).lexically_normal().string();
    }

}

void wizardDraw(App& app) {
    Wizard& w = app.wizard;

    {
        std::lock_guard<std::mutex> lock(w.browseMutex);

        if (w.browsedReady) {
            w.firmware = w.browsed;
            w.browsedReady = false;
            followName(app);
        }
    }

    if (w.open && !ImGui::IsPopupOpen("##wizard")) ImGui::OpenPopup("##wizard");

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(std::min(940.0f, vp->WorkSize.x - 40), std::min(640.0f, vp->WorkSize.y - 40));
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24, 20});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.15f, 0.15f, 0.17f, 1));

    const bool shown = ImGui::BeginPopupModal("##wizard", nullptr,
                                              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    if (!shown) return;

    header(app);
    ImGui::BeginChild("##body", {0, -48}, 0, ImGuiWindowFlags_NoScrollbar);

    switch (w.step) {
    case 0: deviceStep(app); break;
    case 1: firmwareStep(app); break;
    case 2: optionsStep(app); break;
    default: summaryStep(app); break;
    }

    ImGui::EndChild();
    ImGui::Separator();
    ImGui::Dummy({0, 2});

    bool close = false;

    if (pillButton("Cancel", Icon::Close, false) || ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;

    if (!w.error.empty()) {
        ImGui::SameLine(0, 16);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 7);
        ImGui::TextColored(kRed, "%s", w.error.c_str());
    }

    const bool last = w.step == 3;
    const char* next = last ? (w.edit ? "Rebuild" : "Create") : "Continue";
    const float nextW = ImGui::CalcTextSize(next).x + 50;
    const float backW = ImGui::CalcTextSize("Back").x + 50;
    ImGui::SameLine(ImGui::GetWindowWidth() - 24 - nextW - (w.step > 0 ? backW + 8 : 0));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY());

    if (w.step > 0) {
        if (pillButton("Back", Icon::Back, false)) --w.step;
        ImGui::SameLine(0, 8);
    }

    ImGui::BeginDisabled(!canContinue(app));

    if (pillButton(next, last ? (w.edit ? Icon::Refresh : Icon::Check) : Icon::Forward, true) ||
        (ImGui::IsKeyPressed(ImGuiKey_Enter) && canContinue(app) && !ImGui::IsAnyItemActive())) {
        if (!last) {
            ++w.step;
        } else {
            std::string error;
            const BuildRequest r = request(app);

            if (app.machines->build(r, error)) {
                close = true;
                // Listed once the scan sees its directory, a moment from now.
                app.pane = Pane::Instance;
                app.selected = r.name;
                app.instances = app.machines->list();
                selectInstance(app, r.name);
            } else {
                w.error = error;
            }
        }
    }

    ImGui::EndDisabled();

    if (close) {
        w.open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}
