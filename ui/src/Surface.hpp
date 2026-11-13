#pragma once

#include "Layout.hpp"
#include "Touch.hpp"

#include "imgui.h"

#include <string>
#include <unordered_map>
#include <vector>

struct Fonts {
    ImFont* ui = nullptr;
    ImFont* label = nullptr;  // condensed bold, the way panels are printed
    ImFont* title = nullptr;  // model names
};

enum class View {
    Drawn,  // everything drawn from the layout
    Photo,  // the product photo, with the screen and pressed controls drawn over it
    Trace,  // the drawing over the photo, for checking a layout against it
};

struct SurfaceOptions {
    View view = View::Drawn;
    float traceAlpha = 0.6f;

    // Light the lamps a layout marks "lit", as a stand-in for the guest
    // driving them.
    bool layoutLamps = true;
    bool animateMeters = true;
    bool showIds = false;

    // One per face of the layout, in the same order; an invalid id for a
    // face with no photo.
    struct Photo {
        ImTextureID id = ImTextureID_Invalid;
        ImVec2 size = {0, 0};
    };
    std::vector<Photo> photos;

    // The guest's display, drawn in the lcd; an invalid id for none.
    Photo screen;
};

// Where the panel's screen is being pointed at, 0 to 1 across the guest's
// display, and whether it is pressed.
struct ScreenPointer {
    bool inside = false;
    bool down = false;
    ImVec2 uv = {0, 0};
};

// What a control is doing right now. Kept by element id, so it survives a
// layout reload.
struct ControlState {
    float value = 0.0f;  // knobs and faders, 0 to 1
    float angle = 0.0f;  // encoders and jogs, radians clockwise
    float spin = 0.0f;   // jog velocity, radians a second
    float flash = 0.0f;  // pads, fades after a hit
    int quadrant = -1;   // the quarter of a split pad last hit, 0 to 3
    bool pressed = false;
    bool lit = false;
    // A colour the guest lit the lamp in, over the layout's; alpha 0 for none.
    ImVec4 lamp = {0, 0, 0, 0};
};

class Surface {
public:
    // Keeps the state of controls whose ids are still there.
    void setLayout(const Layout* layout);

    // Fills the current window's remaining space with the device and handles
    // the mouse on it.
    void draw(const SurfaceOptions& options, const Fonts& fonts);

    // The control under the mouse, or null.
    const Element* hovered() const;

    // What every control is doing, by id, for the guest link to read and to
    // light lamps in.
    std::unordered_map<std::string, ControlState>& states() { return state_; }
    const Layout* layout() const { return layout_; }

    // The mouse or one finger on the lcd, for the guest's screen.
    const ScreenPointer& screenPointer() const { return screenPointer_; }

    // A scroll, as the finger moved it, in screen pixels with y down. macOS
    // turns a finger dragged on a touchscreen or on Sidecar into a scroll
    // rather than a mouse drag, so this is what turns a knob by touch.
    void scroll(ImVec2 finger) { scroll_ += finger; }

    // A finger, handled at the next draw. Each one holds its own control, so
    // two hands can work two jogs at once.
    void touch(const TouchEvent& e) { touches_.push_back(e); }

    // Where the device was drawn last frame, in window points. A touch that
    // lands outside it goes to the rest of the window instead.
    bool contains(ImVec2 p) const {
        return p.x >= canvasMin_.x && p.y >= canvasMin_.y && p.x < canvasMax_.x && p.y < canvasMax_.y;
    }

private:
    // Something holding a control: the mouse, or one finger.
    struct Contact {
        int index = -1;
        ImVec2 start{0, 0};  // screen pixels
        ImVec2 pos{0, 0};
        float value = 0.0f;
        float angle = 0.0f;
        float lastJogAngle = 0.0f;
    };

    static constexpr uint64_t kMouse = ~uint64_t(0);

    int elementAt(ImVec2 screen) const;
    void begin(uint64_t who, int index, ImVec2 at);
    void drag(Contact& c, ImVec2 at);
    void end(uint64_t who);
    void touches();
    void scrolled(int index, ImVec2 finger);
    void reset(int index);
    bool hit(const Element& e, ImVec2 mouseMm) const;
    bool onScreen(ImVec2 at, ImVec2* uv = nullptr) const;

    const Layout* layout_ = nullptr;
    std::unordered_map<std::string, ControlState> state_;

    int hovered_ = -1;
    std::unordered_map<uint64_t, Contact> contacts_;
    std::vector<TouchEvent> touches_;
    ImVec2 scroll_ = {0, 0};

    // The lcd's picture, in screen pixels from the last draw, and who holds
    // it: kMouse, a finger, or kNobody.
    static constexpr uint64_t kNobody = ~uint64_t(0) - 1;
    ImVec2 screenMin_ = {0, 0};
    ImVec2 screenMax_ = {0, 0};
    uint64_t screenHolder_ = kNobody;
    bool screenLift_ = false;
    ScreenPointer screenPointer_;
    ImVec2 canvasMin_ = {0, 0};
    ImVec2 canvasMax_ = {0, 0};

    // Device to screen, from the last draw.
    ImVec2 origin_ = {0, 0};
    float scale_ = 1.0f;
    double time_ = 0.0;
};
