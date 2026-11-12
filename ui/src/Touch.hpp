#pragma once

// Touchscreen input, one event per finger. SDL reports touchscreens itself on
// Linux and Windows, but its Cocoa backend never sees a finger on macOS 27's
// touchscreens or Sidecar: AppKit hands direct touches only to gesture
// recognizers, and to the window only as clicks made up after a tap ends. A
// drag sends nothing at all. touchStart() attaches a recognizer that reports
// every finger instead.

#include "imgui.h"

#include <cstdint>
#include <vector>

struct SDL_Window;

struct TouchEvent {
    enum Phase { Down, Move, Up };

    Phase phase = Down;
    uint64_t id = 0;   // unique while the finger is down
    ImVec2 pos{0, 0};  // window points, y down
};

// Starts reporting the window's direct touches. False where SDL's own finger
// events are what there is.
bool touchStart(SDL_Window* window);

void touchPush(const TouchEvent& e);

// The touches since the last call, oldest first.
std::vector<TouchEvent> touchDrain();

// Whether a finger is down, or one lifted within the last `ns`. macOS still
// sends the window a click for each tap, after the finger lifts, which this
// tells apart from a real mouse.
bool touchRecent(uint64_t ns);
