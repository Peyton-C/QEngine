#include "Midi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <utility>

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr float kTwoPi = 6.28318530718f;

// One encoder click, in the radians the surface turns a knob by.
constexpr float kEncoderStep = kTwoPi / 24.0f;

// Engine sends lamp colours through gamma 3.5 (JP24_Controller_Device.qml), so
// they are taken back out of it to look right on screen.
constexpr float kLampGamma = 3.5f;

std::string hex(const std::vector<unsigned char>& m, size_t limit = 16) {
    std::string s;
    char b[4];

    for (size_t i = 0; i < m.size() && i < limit; ++i) {
        std::snprintf(b, sizeof b, "%02X ", m[i]);
        s += b;
    }

    if (m.size() > limit) {
        s += "...";
    }

    return s;
}

int messageLength(unsigned char status) {
    const unsigned char kind = status & 0xF0;
    return kind == 0xC0 || kind == 0xD0 ? 2 : 3;
}

bool isPair(const json& cc) { return cc.is_array() && cc.size() == 2; }

// What a knob, fader or jog would send right now, to tell when it changes.
int outputOf(const Element& e, const ControlState& st) {
    const json& m = e.p["midi"];

    if (e.kind == "jog") {
        const float ticks = num(m, "ticks", 1024.0f);
        return int(std::floor(st.angle / kTwoPi * ticks)) & 0x3FFF;
    }

    if (e.kind == "knob" || e.kind == "fader") {
        int v = int(std::lround(std::clamp(st.value, 0.0f, 1.0f) * 16383.0f));

        if (flag(m, "invert")) {
            v = 16383 - v;
        }

        return isPair(m.value("cc", json())) ? v : v >> 7;
    }

    return -1;
}

ImVec4 layoutLamp(const json& p) {
    return colour(p, "lamp", colour(p, "ring", colour(p, "text", {1, 1, 1, 1})));
}

}  // namespace

std::vector<Instance> findInstances(const fs::path& dir) {
    std::vector<Instance> found;
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::ifstream env(entry.path() / "instance.env");

        if (!env) {
            continue;
        }

        std::unordered_map<std::string, std::string> v;
        std::string line;

        while (std::getline(env, line)) {
            const size_t eq = line.find('=');

            if (eq != std::string::npos && line[0] != '#') {
                v[line.substr(0, eq)] = line.substr(eq + 1);
            }
        }

        if (v["DEVICE"] != "engine") {
            continue;
        }

        Instance i;
        i.name = entry.path().filename().string();
        i.product = v["PRODUCT_CODE"];

        // An instance from before MIDI_PORT was recorded is served where
        // run_instance.sh puts it, 4000 above its SSH port.
        try {
            i.port = !v["MIDI_PORT"].empty() ? std::stoi(v["MIDI_PORT"])
                     : !v["SSH_PORT"].empty() ? std::stoi(v["SSH_PORT"]) + 4000
                                              : 0;
        } catch (const std::exception&) {
            i.port = 0;
        }

        if (i.port > 0) {
            found.push_back(i);
        }
    }

    std::sort(found.begin(), found.end(), [](const Instance& a, const Instance& b) { return a.name < b.name; });
    return found;
}

void GuestLink::connect(int port) {
    if (port != port_) {
        close();
    }

    port_ = port;
    want_ = true;
    lastTry_ = -10.0;
}

void GuestLink::disconnect() {
    want_ = false;
    close();
}

void GuestLink::say(const std::string& line) {
    log_.push_back(line);

    while (log_.size() > 200) {
        log_.pop_front();
    }
}

#ifdef _WIN32
void GuestLink::open(double) { problem_ = "not available on Windows yet"; }
void GuestLink::opened(double) {}
void GuestLink::close() {}
void GuestLink::send(std::initializer_list<unsigned char>) {}
void GuestLink::receive(Surface&) {}
#else
// Non-blocking, and finished by opened() on a later frame. A refused port is
// answered at once, but a connect to QEMU can also hang unanswered: its socket
// takes one client at a time, and it only notices that client has gone when it
// reads, which it does only while something in the guest has the port open. A
// blocking connect then froze the whole window until the system gave up.
void GuestLink::open(double now) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return;
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port_));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 && errno != EINPROGRESS) {
        problem_ = "nothing on :" + std::to_string(port_) + ", is the instance running?";
        ::close(fd);
        return;
    }

    connecting_ = fd;
    connectStart_ = now;
    opened(now);
}

void GuestLink::opened(double now) {
    pollfd p = {connecting_, POLLOUT, 0};

    if (::poll(&p, 1, 0) <= 0) {
        if (now - connectStart_ > 3.0) {
            problem_ = "QEMU did not answer on :" + std::to_string(port_) +
                       ". It holds an earlier connection until the guest reads the port, so the guest "
                       "may have no midibridge running.";
            ::close(connecting_);
            connecting_ = -1;
        }
        return;
    }

    int err = 0;
    socklen_t len = sizeof err;
    getsockopt(connecting_, SOL_SOCKET, SO_ERROR, &err, &len);
    const int fd = std::exchange(connecting_, -1);

    if (err != 0) {
        problem_ = "nothing on :" + std::to_string(port_) + ", is the instance running?";
        ::close(fd);
        return;
    }

    // A button press is three bytes, and waiting to fill a packet with more
    // would be latency on every one.
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    fd_ = fd;
    problem_.clear();
    pending_.clear();
    status_ = 0;
    clearLamps_ = true;
    say("connected to localhost:" + std::to_string(port_));
}

void GuestLink::close() {
    if (connecting_ >= 0) {
        ::close(connecting_);
        connecting_ = -1;
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        say("disconnected");
    }
}

void GuestLink::send(std::initializer_list<unsigned char> bytes) {
    if (fd_ < 0) {
        return;
    }

#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const std::vector<unsigned char> m(bytes);

    if (::send(fd_, m.data(), m.size(), flags) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        close();
        return;
    }

    ++sent_;
    say("> " + hex(m));
}

void GuestLink::receive(Surface& surface) {
    unsigned char buf[4096];

    for (;;) {
        const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);

        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close();
            return;
        }

        if (n < 0) {
            return;
        }

        for (ssize_t i = 0; i < n; ++i) {
            const unsigned char b = buf[i];

            if (b >= 0xF8) {
                continue;  // clock and other real time bytes
            }

            if (b == 0xF0) {
                pending_ = {b};
                status_ = 0xF0;
            } else if (b == 0xF7) {
                if (status_ == 0xF0) {
                    pending_.push_back(b);
                    message(surface, pending_);
                }
                pending_.clear();
                status_ = 0;
            } else if (status_ == 0xF0) {
                if (pending_.size() < 4096) {
                    pending_.push_back(b);
                }
            } else if (b & 0x80) {
                status_ = b < 0xF0 ? b : 0;
                pending_ = {b};
            } else if (status_) {
                if (pending_.empty()) {
                    pending_ = {status_};
                }
                pending_.push_back(b);

                if (int(pending_.size()) == messageLength(status_)) {
                    message(surface, pending_);
                    pending_.clear();
                }
            }
        }
    }
}
#endif

void GuestLink::message(Surface& surface, const std::vector<unsigned char>& m) {
    ++received_;
    say("< " + hex(m));
    auto& states = surface.states();
    const Layout* layout = surface.layout();

    if (!layout) {
        return;
    }

    auto light = [&](int ch, int note, auto&& set) {
        auto it = lamps_.find(ch << 8 | note);

        if (it == lamps_.end()) {
            return;
        }

        for (const std::string& id : it->second) {
            set(states[id], layout->elements[size_t(controls_[id].index)]);
        }
    };

    const unsigned char kind = m[0] & 0xF0;

    if ((kind == 0x90 || kind == 0x80) && m.size() == 3) {
        const int velocity = kind == 0x80 ? 0 : m[2];

        light(m[0] & 0x0F, m[1], [&](ControlState& st, const Element& e) {
            st.lit = velocity > 0;
            const ImVec4 c = layoutLamp(e.p);
            st.lamp = velocity > 0 && velocity < 64 ? ImVec4(c.x * 0.35f, c.y * 0.35f, c.z * 0.35f, 1) : ImVec4();
        });
        return;
    }

    // inMusic's RGB lamp: F0 00 02 0B 7F <product> 03 00 05 <ch> <note> <r> <g> <b> F7
    if (m.size() == 15 && m[1] == 0x00 && m[2] == 0x02 && m[3] == 0x0B && m[6] == 0x03 && m[8] == 0x05) {
        const auto level = [](unsigned char v) { return std::pow(float(v) / 127.0f, 1.0f / kLampGamma); };
        const ImVec4 c = {level(m[11]), level(m[12]), level(m[13]), 1};

        light(m[9], m[10], [&](ControlState& st, const Element&) {
            st.lit = m[11] || m[12] || m[13];
            st.lamp = st.lit ? c : ImVec4();
        });
    }
}

void GuestLink::note(int ch, int n, bool on, int velocity) {
    if (on) {
        send({(unsigned char)(0x90 | ch), (unsigned char)n, (unsigned char)velocity});
    } else {
        send({(unsigned char)(0x80 | ch), (unsigned char)n, 0});
    }
}

// Most significant half first, so Engine has both by the time the second
// arrives.
void GuestLink::cc(int ch, const json& c, int value) {
    if (isPair(c)) {
        send({(unsigned char)(0xB0 | ch), (unsigned char)c[0].get<int>(), (unsigned char)(value >> 7)});
        send({(unsigned char)(0xB0 | ch), (unsigned char)c[1].get<int>(), (unsigned char)(value & 0x7F)});
    } else if (c.is_number()) {
        send({(unsigned char)(0xB0 | ch), (unsigned char)c.get<int>(), (unsigned char)value});
    }
}

void GuestLink::bind(Surface& surface) {
    const Layout* layout = surface.layout();

    if (layout == layout_ && (!layout || layout->mtime == mtime_)) {
        return;
    }

    layout_ = layout;
    controls_.clear();
    lamps_.clear();

    if (!layout) {
        return;
    }

    mtime_ = layout->mtime;
    auto& states = surface.states();

    for (size_t i = 0; i < layout->elements.size(); ++i) {
        const Element& e = layout->elements[i];

        if (e.id.empty() || !e.p.contains("midi") || !e.p["midi"].is_object()) {
            continue;
        }

        const json& m = e.p["midi"];
        Control c;
        c.index = int(i);
        c.ch = std::clamp(m.value("ch", layout->midiChannel), 0, 15);
        c.prev = states[e.id];
        c.lastValue = outputOf(e, c.prev);
        controls_[e.id] = c;

        if (m.contains("note")) {
            lamps_[c.ch << 8 | m["note"].get<int>()].push_back(e.id);
        }
    }

    clearLamps_ = connected();
}

// What one control does between last frame and this one, as MIDI.
void GuestLink::track(Control& c, const ControlState& st, const Element& e) {
    const json& m = e.p["midi"];
    const int ch = c.ch;
    const bool down = st.pressed && !c.prev.pressed;
    const bool up = !st.pressed && c.prev.pressed;
    const int n = m.value("note", -1);

    if (e.kind == "button" || e.kind == "pad" || e.kind == "footswitch") {
        if (n >= 0 && (down || up)) {
            note(ch, n, down);
        }
    } else if (e.kind == "switch") {
        const bool on = st.value >= 0.5f;

        if (n >= 0 && on != (c.prev.value >= 0.5f)) {
            note(ch, n, on);
        }
    } else if (e.kind == "encoder") {
        c.turned += (st.angle - c.prev.angle) / kEncoderStep;

        for (; c.turned >= 1.0f; c.turned -= 1.0f) cc(ch, m.value("cc", json()), 1);
        for (; c.turned <= -1.0f; c.turned += 1.0f) cc(ch, m.value("cc", json()), 127);

        // Dragging turns an encoder, so only a tap that leaves it where it
        // was pushes it.
        if (down) {
            c.pressAngle = st.angle;
        }

        if (up && n >= 0 && std::fabs(st.angle - c.pressAngle) < 0.05f) {
            note(ch, n, true);
            note(ch, n, false);
        }
    } else if (e.kind == "jog") {
        if (n >= 0 && (down || up)) {
            note(ch, n, down);
        }

        const int v = outputOf(e, st);

        if (v != c.lastValue) {
            cc(ch, m.value("cc", json()), v);
            c.lastValue = v;
        }
    } else if (e.kind == "knob" || e.kind == "fader") {
        const int v = outputOf(e, st);

        if (v != c.lastValue) {
            cc(ch, m.value("cc", json()), v);
            c.lastValue = v;
        }
    }
}

void GuestLink::update(Surface& surface, double now) {
    if (connecting_ >= 0) {
        opened(now);
    } else if (want_ && fd_ < 0 && now - lastTry_ >= 2.0) {
        lastTry_ = now;
        open(now);
    }

    bind(surface);

    if (fd_ >= 0) {
        receive(surface);
    }

    const Layout* layout = surface.layout();

    if (!layout) {
        return;
    }

    auto& states = surface.states();

    // The guest drives every lamp it has a note for, starting from dark,
    // rather than leaving the layout's "lit" showing.
    if (clearLamps_) {
        for (auto& [id, c] : controls_) {
            states[id].lit = false;
            states[id].lamp = ImVec4();
        }
        clearLamps_ = false;
    }

    for (auto& [id, c] : controls_) {
        ControlState& st = states[id];

        // Moves made while disconnected are not sent later, all at once.
        if (fd_ >= 0) {
            track(c, st, layout->elements[size_t(c.index)]);
        } else {
            c.lastValue = outputOf(layout->elements[size_t(c.index)], st);
            c.turned = 0.0f;
        }

        c.prev = st;
    }
}
