#pragma once

#include <SDL3/SDL.h>

#include "imgui.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The guest's display, read over VNC and shown in the panel's lcd.
//
// An instance's screen is on one of two VNC servers on localhost. QEMU serves
// its console on 5900 + VNC_DISPLAY when run with the vnc or egl-vnc display,
// which is the default on macOS. A legacy rootfs has Engine draw through its
// own Qt VNC server instead, forwarded to ENGINE_VNC_PORT, and QEMU's console
// there is only a text console.
//
// This is a minimal RFB client rather than libvncclient, which is GPL and
// would make the UI GPL with it. Both servers are on localhost, so Raw pixels
// cost nothing worth compressing away, and Raw and CopyRect are all it asks
// for. It speaks 3.8 to QEMU and 3.3 to Qt, which offers nothing newer, and
// accepts only the None security type, which both use.
//
// A thread reads the socket, since a framebuffer update is read whole or not
// at all. The render thread uploads what changed with upload() and sends the
// pointer with pointer().
class GuestScreen {
public:
    ~GuestScreen();

    // Keeps trying the port, once a second, until disconnect().
    void connect(int port);
    void disconnect();

    bool wanted() const { return port_ != 0; }
    int port() const { return port_; }
    bool connected() const { return state_ == State::Live; }
    std::string status() const;

    // Before ImGui::NewFrame(): makes the texture the size the guest is now.
    void prepare(SDL_GPUDevice* device);
    // In the frame's command buffer, before its render pass: copies what the
    // guest redrew since the last frame.
    void upload(SDL_GPUCommandBuffer* cmd);
    void release();

    ImTextureID texture() const { return texture_ ? (ImTextureID)(intptr_t)texture_ : ImTextureID_Invalid; }
    ImVec2 size() const { return {float(texWidth_), float(texHeight_)}; }

    // Where the pointer is, 0 to 1 across the screen, and whether it is
    // pressed. Sent only when it changes. The guest's usb-tablet is absolute,
    // so a press lands where it is drawn, which Engine reads as a touch.
    void pointer(ImVec2 uv, bool down);

private:
    enum class State { Idle, Connecting, Live, Retrying };

    void run(int port);
    bool session(int fd);
    bool readUpdate(int fd);
    bool send(const void* data, size_t len);
    void setStatus(State state, const std::string& text);
    void stopThread();

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> fd_{-1};
    std::atomic<State> state_{State::Idle};
    int port_ = 0;
    std::mutex sendMutex_;

    // Written by the reader, taken by upload(). dirty_ is x0, y0, x1, y1.
    mutable std::mutex mutex_;
    std::string status_;
    std::vector<uint32_t> fb_;
    int width_ = 0;
    int height_ = 0;
    int dirty_[4] = {0, 0, 0, 0};
    std::vector<uint8_t> scratch_;  // the reader's, for one rectangle

    // The render thread's.
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* texture_ = nullptr;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    int texWidth_ = 0;
    int texHeight_ = 0;
    bool fresh_ = false;  // a new texture, which needs the whole frame
    int lastX_ = -1;
    int lastY_ = -1;
    bool lastDown_ = false;
};
