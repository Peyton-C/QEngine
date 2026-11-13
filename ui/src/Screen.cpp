#include "Screen.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

// A server asking for anything bigger than this has sent garbage.
constexpr int kMaxSide = 8192;

constexpr int32_t kRaw = 0;
constexpr int32_t kCopyRect = 1;
constexpr int32_t kDesktopSize = -223;

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

void put32(uint8_t* p, uint32_t v) {
    put16(p, uint16_t(v >> 16));
    put16(p + 2, uint16_t(v));
}

#ifndef _WIN32
bool readAll(int fd, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);

    while (len > 0) {
        const ssize_t n = ::recv(fd, p, len, 0);

        if (n <= 0) {
            return false;
        }

        p += n;
        len -= size_t(n);
    }

    return true;
}

bool skip(int fd, size_t len) {
    uint8_t buf[512];

    while (len > 0) {
        const size_t n = std::min(len, sizeof buf);

        if (!readAll(fd, buf, n)) {
            return false;
        }

        len -= n;
    }

    return true;
}

// The reason string a server sends with a refusal.
std::string readReason(int fd) {
    uint8_t len[4];

    if (!readAll(fd, len, 4)) {
        return "";
    }

    std::string s(std::min<uint32_t>(be32(len), 1024), '\0');
    return readAll(fd, s.data(), s.size()) ? s : "";
}
#endif

}  // namespace

GuestScreen::~GuestScreen() {
    stopThread();
    release();
}

void GuestScreen::connect(int port) {
    if (port == port_ && thread_.joinable()) {
        return;
    }

    stopThread();
    port_ = port;
    stop_ = false;
    setStatus(State::Connecting, "connecting");
    thread_ = std::thread(&GuestScreen::run, this, port);
}

void GuestScreen::disconnect() {
    stopThread();
    port_ = 0;
    setStatus(State::Idle, "");
}

std::string GuestScreen::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void GuestScreen::setStatus(State state, const std::string& text) {
    std::lock_guard lock(mutex_);
    state_ = state;
    status_ = text;
}

void GuestScreen::stopThread() {
    stop_ = true;

    // The reader is blocked in recv(), and shutting the socket down is what
    // wakes it. Under sendMutex_, so the reader cannot close the descriptor,
    // and have its number reused, in between.
#ifndef _WIN32
    {
        std::lock_guard lock(sendMutex_);

        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
        }
    }
#endif

    if (thread_.joinable()) {
        thread_.join();
    }
}

bool GuestScreen::send(const void* data, size_t len) {
#ifndef _WIN32
    std::lock_guard lock(sendMutex_);
    const int fd = fd_;
    auto* p = static_cast<const uint8_t*>(data);

    while (fd >= 0 && len > 0) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, p, len, 0);
#endif
        if (n <= 0) {
            return false;
        }

        p += n;
        len -= size_t(n);
    }

    return fd >= 0;
#else
    (void)data;
    (void)len;
    return false;
#endif
}

void GuestScreen::run(int port) {
#ifndef _WIN32
    while (!stop_) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        fd_ = fd;

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(uint16_t(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        // Localhost answers a connect at once, accepted or refused, so a
        // blocking one is fine here, unlike MIDI's chardev.
        std::string why;

        if (stop_) {
            why = "stopped";
        } else if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
            why = std::string("nothing on :") + std::to_string(port) + ", " + std::strerror(errno);
        } else if (!session(fd)) {
            why = status();
        }

        {
            std::lock_guard lock(sendMutex_);
            ::close(fd_.exchange(-1));
        }

        if (stop_) {
            break;
        }

        setStatus(State::Retrying, why);

        for (int i = 0; i < 20 && !stop_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
#else
    (void)port;
    setStatus(State::Retrying, "the guest screen is not supported on Windows yet");
#endif
}

bool GuestScreen::session(int fd) {
#ifndef _WIN32
    const auto fail = [&](const std::string& why) {
        setStatus(State::Retrying, why);
        return false;
    };

    char version[13] = {};

    if (!readAll(fd, version, 12)) {
        return fail("closed before it said hello");
    }

    int major = 0;
    int minor = 0;

    if (std::sscanf(version, "RFB %3d.%3d", &major, &minor) != 2 || major != 3) {
        return fail("not a VNC server");
    }

    // QEMU offers 3.8, Qt's server 3.3. 3.7 and 3.8 differ only in whether
    // the None security type is answered with a result.
    minor = minor >= 8 ? 8 : minor == 7 ? 7 : 3;
    char reply[13];
    std::snprintf(reply, sizeof reply, "RFB 003.%03d\n", minor);
    send(reply, 12);

    if (minor == 3) {
        uint8_t type[4];

        if (!readAll(fd, type, 4)) {
            return fail("closed during the handshake");
        }

        if (be32(type) == 0) {
            return fail("refused: " + readReason(fd));
        }

        if (be32(type) != 1) {
            return fail("asks for a password, which the UI does not do");
        }
    } else {
        uint8_t count = 0;

        if (!readAll(fd, &count, 1)) {
            return fail("closed during the handshake");
        }

        if (count == 0) {
            return fail("refused: " + readReason(fd));
        }

        uint8_t types[255];

        if (!readAll(fd, types, count)) {
            return fail("closed during the handshake");
        }

        if (std::find(types, types + count, uint8_t(1)) == types + count) {
            return fail("asks for a password, which the UI does not do");
        }

        const uint8_t none = 1;
        send(&none, 1);

        if (minor == 8) {
            uint8_t result[4];

            if (!readAll(fd, result, 4)) {
                return fail("closed during the handshake");
            }

            if (be32(result) != 0) {
                return fail("refused: " + readReason(fd));
            }
        }
    }

    // Shared, so a VNC viewer can stay connected beside the UI.
    const uint8_t shared = 1;
    send(&shared, 1);

    uint8_t init[24];

    if (!readAll(fd, init, 24)) {
        return fail("closed during the handshake");
    }

    std::string name(std::min<uint32_t>(be32(init + 20), 1024), '\0');

    if (!readAll(fd, name.data(), name.size())) {
        return fail("closed during the handshake");
    }

    const int w = be16(init);
    const int h = be16(init + 2);

    if (w > kMaxSide || h > kMaxSide) {
        return fail("sent a nonsense size");
    }

    {
        std::lock_guard lock(mutex_);
        width_ = w;
        height_ = h;
        fb_.assign(size_t(w) * size_t(h), 0xFF000000u);
        dirty_[0] = 0;
        dirty_[1] = 0;
        dirty_[2] = w;
        dirty_[3] = h;
        state_ = State::Live;
        status_ = std::to_string(w) + " x " + std::to_string(h) + (name.empty() ? "" : ", " + name);
    }

    // 32-bit little endian with red lowest, so each pixel is R, G, B, X in
    // memory, the texture's own layout. The X byte is made opaque on arrival.
    uint8_t format[20] = {0, 0, 0, 0, 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 0, 8, 16, 0, 0, 0};
    send(format, sizeof format);

    const int32_t encodings[] = {kCopyRect, kRaw, kDesktopSize};
    uint8_t enc[4 + sizeof encodings] = {2, 0};
    put16(enc + 2, uint16_t(std::size(encodings)));

    for (size_t i = 0; i < std::size(encodings); ++i) {
        put32(enc + 4 + i * 4, uint32_t(encodings[i]));
    }

    send(enc, sizeof enc);

    // Asks for the whole screen, then only for what changes. A server
    // answers an incremental request when it has something, so this does not
    // spin while the guest is idle.
    const auto request = [&](bool incremental) {
        uint8_t m[10] = {3, uint8_t(incremental)};
        std::lock_guard lock(mutex_);
        put16(m + 6, uint16_t(width_));
        put16(m + 8, uint16_t(height_));
        return send(m, sizeof m);
    };

    request(false);

    while (!stop_) {
        uint8_t type = 0;

        if (!readAll(fd, &type, 1)) {
            return fail("the guest closed the connection");
        }

        switch (type) {
        case 0:
            if (!readUpdate(fd)) {
                return fail(state_ == State::Live ? "lost the connection mid-frame" : status());
            }

            request(true);
            break;
        case 1: {  // a colour map, meaningless in true colour
            uint8_t m[5];

            if (!readAll(fd, m, 5) || !skip(fd, size_t(be16(m + 3)) * 6)) {
                return fail("the guest closed the connection");
            }
            break;
        }
        case 2:  // bell
            break;
        case 3: {  // the guest's clipboard
            uint8_t m[7];

            if (!readAll(fd, m, 7) || !skip(fd, be32(m + 3))) {
                return fail("the guest closed the connection");
            }
            break;
        }
        default:
            return fail("sent message " + std::to_string(type) + ", which the UI does not know");
        }
    }

    return false;
#else
    (void)fd;
    return false;
#endif
}

bool GuestScreen::readUpdate(int fd) {
#ifndef _WIN32
    uint8_t head[3];

    if (!readAll(fd, head, 3)) {
        return false;
    }

    const int rects = be16(head + 1);

    for (int i = 0; i < rects; ++i) {
        uint8_t r[12];

        if (!readAll(fd, r, 12)) {
            return false;
        }

        const int x = be16(r);
        const int y = be16(r + 2);
        const int w = be16(r + 4);
        const int h = be16(r + 6);
        const auto encoding = int32_t(be32(r + 8));

        if (encoding == kDesktopSize) {
            if (w > kMaxSide || h > kMaxSide) {
                setStatus(State::Retrying, "sent a nonsense size");
                return false;
            }

            std::lock_guard lock(mutex_);
            width_ = w;
            height_ = h;
            fb_.assign(size_t(w) * size_t(h), 0xFF000000u);
            dirty_[0] = 0;
            dirty_[1] = 0;
            dirty_[2] = w;
            dirty_[3] = h;
            const size_t comma = status_.find(',');
            status_ = std::to_string(w) + " x " + std::to_string(h) +
                      (comma == std::string::npos ? "" : status_.substr(comma));
            continue;
        }

        uint8_t src[4];

        if (encoding == kRaw) {
            scratch_.resize(size_t(w) * size_t(h) * 4);

            if (!readAll(fd, scratch_.data(), scratch_.size())) {
                return false;
            }
        } else if (encoding == kCopyRect) {
            if (!readAll(fd, src, 4)) {
                return false;
            }
        } else {
            setStatus(State::Retrying, "sent encoding " + std::to_string(encoding) + ", which was not asked for");
            return false;
        }

        std::lock_guard lock(mutex_);

        if (x + w > width_ || y + h > height_) {
            continue;  // stale, from before a resize
        }

        if (encoding == kRaw) {
            for (int row = 0; row < h; ++row) {
                uint32_t* dst = fb_.data() + size_t(y + row) * size_t(width_) + size_t(x);
                std::memcpy(dst, scratch_.data() + size_t(row) * size_t(w) * 4, size_t(w) * 4);

                for (int col = 0; col < w; ++col) {
                    dst[col] |= 0xFF000000u;
                }
            }
        } else {
            const int sx = be16(src);
            const int sy = be16(src + 2);

            if (sx + w > width_ || sy + h > height_) {
                continue;
            }

            // Source and destination can overlap, as when a list scrolls.
            std::vector<uint32_t> copy(size_t(w) * size_t(h));

            for (int row = 0; row < h; ++row) {
                std::memcpy(copy.data() + size_t(row) * size_t(w),
                            fb_.data() + size_t(sy + row) * size_t(width_) + size_t(sx), size_t(w) * 4);
            }

            for (int row = 0; row < h; ++row) {
                std::memcpy(fb_.data() + size_t(y + row) * size_t(width_) + size_t(x),
                            copy.data() + size_t(row) * size_t(w), size_t(w) * 4);
            }
        }

        if (dirty_[2] <= dirty_[0]) {
            dirty_[0] = x;
            dirty_[1] = y;
            dirty_[2] = x + w;
            dirty_[3] = y + h;
        } else {
            dirty_[0] = std::min(dirty_[0], x);
            dirty_[1] = std::min(dirty_[1], y);
            dirty_[2] = std::max(dirty_[2], x + w);
            dirty_[3] = std::max(dirty_[3], y + h);
        }
    }

    return true;
#else
    (void)fd;
    return false;
#endif
}

void GuestScreen::prepare(SDL_GPUDevice* device) {
    device_ = device;
    int w = 0;
    int h = 0;

    {
        std::lock_guard lock(mutex_);
        w = width_;
        h = height_;
    }

    if (w == texWidth_ && h == texHeight_) {
        return;
    }

    // SDL keeps a released texture alive until the frames using it finish.
    release();

    if (w <= 0 || h <= 0) {
        return;
    }

    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = Uint32(w);
    info.height = Uint32(h);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    texture_ = SDL_CreateGPUTexture(device, &info);

    SDL_GPUTransferBufferCreateInfo tb = {};
    tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tb.size = Uint32(w) * Uint32(h) * 4;
    transfer_ = texture_ ? SDL_CreateGPUTransferBuffer(device, &tb) : nullptr;

    if (!transfer_) {
        release();
        return;
    }

    texWidth_ = w;
    texHeight_ = h;
    fresh_ = true;
}

void GuestScreen::upload(SDL_GPUCommandBuffer* cmd) {
    if (!texture_) {
        return;
    }

    int x0 = 0;
    int y0 = 0;
    int w = 0;
    int h = 0;

    {
        std::lock_guard lock(mutex_);

        // Resized since prepare(); the next frame's texture will fit.
        if (width_ != texWidth_ || height_ != texHeight_) {
            return;
        }

        if (fresh_) {
            dirty_[0] = 0;
            dirty_[1] = 0;
            dirty_[2] = width_;
            dirty_[3] = height_;
            fresh_ = false;
        }

        x0 = dirty_[0];
        y0 = dirty_[1];
        w = dirty_[2] - x0;
        h = dirty_[3] - y0;

        if (w <= 0 || h <= 0) {
            return;
        }

        // Only the changed rectangle crosses to the GPU, packed.
        auto* dst = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, transfer_, true));

        if (!dst) {
            return;
        }

        for (int row = 0; row < h; ++row) {
            std::memcpy(dst + size_t(row) * size_t(w) * 4,
                        fb_.data() + size_t(y0 + row) * size_t(width_) + size_t(x0), size_t(w) * 4);
        }

        SDL_UnmapGPUTransferBuffer(device_, transfer_);
        dirty_[0] = dirty_[1] = dirty_[2] = dirty_[3] = 0;
    }

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = transfer_;
    src.pixels_per_row = Uint32(w);
    src.rows_per_layer = Uint32(h);

    SDL_GPUTextureRegion region = {};
    region.texture = texture_;
    region.x = Uint32(x0);
    region.y = Uint32(y0);
    region.w = Uint32(w);
    region.h = Uint32(h);
    region.d = 1;

    SDL_UploadToGPUTexture(copy, &src, &region, false);
    SDL_EndGPUCopyPass(copy);
}

void GuestScreen::release() {
    if (texture_) {
        SDL_ReleaseGPUTexture(device_, texture_);
    }

    if (transfer_) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    }

    texture_ = nullptr;
    transfer_ = nullptr;
    texWidth_ = 0;
    texHeight_ = 0;
}

void GuestScreen::pointer(ImVec2 uv, bool down) {
    if (!connected() || texWidth_ <= 0) {
        return;
    }

    const int x = std::clamp(int(uv.x * float(texWidth_)), 0, texWidth_ - 1);
    const int y = std::clamp(int(uv.y * float(texHeight_)), 0, texHeight_ - 1);

    if (x == lastX_ && y == lastY_ && down == lastDown_) {
        return;
    }

    lastX_ = x;
    lastY_ = y;
    lastDown_ = down;

    uint8_t m[6] = {5, uint8_t(down ? 1 : 0)};
    put16(m + 2, uint16_t(x));
    put16(m + 4, uint16_t(y));
    send(m, sizeof m);
}
