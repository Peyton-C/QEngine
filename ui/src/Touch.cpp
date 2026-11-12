#include "Touch.hpp"

#include <SDL3/SDL.h>

#include <unordered_set>

namespace {
std::vector<TouchEvent> queue;
std::unordered_set<uint64_t> down;
uint64_t last = 0;
}

void touchPush(const TouchEvent& e) {
    queue.push_back(e);
    last = SDL_GetTicksNS();

    if (e.phase == TouchEvent::Down) {
        down.insert(e.id);
    } else if (e.phase == TouchEvent::Up) {
        down.erase(e.id);
    }
}

std::vector<TouchEvent> touchDrain() {
    std::vector<TouchEvent> out;
    out.swap(queue);
    return out;
}

bool touchRecent(uint64_t ns) {
    return !down.empty() || (last != 0 && SDL_GetTicksNS() - last < ns);
}

#ifndef __APPLE__
bool touchStart(SDL_Window*) {
    return false;
}
#endif
