#pragma once

#include "Surface.hpp"

#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// The front panel played into a running guest, over MIDI.
//
// An Engine instance serves its guest's MIDI on a localhost TCP port, MIDI_PORT
// in its instance.env, as plain MIDI 1.0 bytes both ways. In the guest,
// midibridge hands them to midisurface, which forwards them to Engine as the
// product's own control surface and sends Engine's lamps back (--echo-all). So
// the panel speaks the product's own protocol, in the numbers from its
// assignment file, and Engine cannot tell it from the real one.
//
// A control's "midi" field says what it sends:
//
//   {"note": N}                    button or pad, note on while held
//   {"cc": N}                      knob or fader, 7 bits
//   {"cc": [UPPER, LOWER]}         knob or fader, 14 bits as a pair
//   {"cc": N, "relative": true}    encoder, one step a click as 1 or 127; with
//                                  "note", a tap that does not turn it pushes it
//   {"note": T, "cc": [U, L], "ticks": R}
//                                  jog, touch note T and the platter's position
//                                  as a 14-bit count that runs R a turn
//   "ch"                           its channel, else the placed template's
//                                  midi_channel, else the layout's midi.channel
//   "invert": true                 for a fader whose top is its low end
//   "lamp": N                      the note its lamp is lit on, where that is
//                                  not the note it sends, as a jog's ring is
//   "lamp": [N, N, N, N]           a split pad's quarters, each lit apart
//   "lamp_ch"                      the lamp's channel, if not the control's
//   "lamp_type": "rgb" or "note"   only that kind of lamp message, where an
//                                  RGB lamp and a simple one share a note, as
//                                  a Prime 4 G2's load buttons and pitch LEDs do
//
// A meter's "midi" is {"cc": N}, the CC Engine sends its level on: how many
// of ten LEDs are lit, plus 20 to 110 in tens for the one its peak holds at. A control
// with only a "lamp" sends nothing, as an LED or an indicator does.
//
// Lamps come back as Engine lights the product: a note on the lamp's channel
// and note, lit at 64 and over and dim below, or inMusic's RGB SysEx,
// F0 00 02 0B 7F <product> 03 00 05 <ch> <note> <r> <g> <b> F7.

// An Engine instance under build/instances, and the ports its MIDI and its
// screen are on. The screen is Engine's own Qt VNC server on a legacy rootfs,
// else QEMU's VNC console, which only the vnc and egl-vnc displays serve.
struct Instance {
    std::string name;
    std::string product;
    int port = 0;
    int screenPort = 0;
    bool engineVnc = false;  // the screen is Engine's own server
    std::string display;     // DISPLAY_MODE, empty when the host picks it
};

std::vector<Instance> findInstances(const std::filesystem::path& dir);

class GuestLink {
public:
    ~GuestLink() { close(); }

    // Keeps trying the port, once a second, until disconnect().
    void connect(int port);
    void disconnect();

    bool connected() const { return fd_ >= 0; }
    // Why the last attempt failed, empty while connected or yet to try.
    const std::string& problem() const { return problem_; }
    bool wanted() const { return want_; }
    int port() const { return port_; }

    // Once a frame: lights lamps from what the guest sent, and sends what moved.
    void update(Surface& surface, double now);

    const std::deque<std::string>& log() const { return log_; }
    unsigned sent() const { return sent_; }
    unsigned received() const { return received_; }

private:
    struct Control {
        int index = -1;
        int ch = 0;
        ControlState prev;
        float turned = 0.0f;  // encoder steps not yet sent
        float pressAngle = 0.0f;
        int lastValue = -1;  // what a knob, fader or jog last sent
    };

    void open(double now);
    void opened(double now);
    void close();
    void bind(Surface& surface);
    void receive(Surface& surface);
    void message(Surface& surface, const std::vector<unsigned char>& m);
    void send(std::initializer_list<unsigned char> bytes);
    void track(Control& c, const ControlState& st, const Element& e);
    void note(int ch, int note, bool on, int velocity = 127);
    void cc(int ch, const nlohmann::json& cc, int value14);
    void say(const std::string& line);

    int fd_ = -1;
    int connecting_ = -1;  // a connect not yet answered
    double connectStart_ = 0.0;
    std::string problem_;
    int port_ = 0;
    bool want_ = false;
    double lastTry_ = -10.0;

    // The layout the controls below were read from, to notice a reload.
    const Layout* layout_ = nullptr;
    std::filesystem::file_time_type mtime_;
    std::unordered_map<std::string, Control> controls_;
    // What a lamp message lights, by ch << 8 | note: a control, and which
    // quarter of it, or -1 for all of it.
    struct Lamp {
        std::string id;
        int quad = -1;
        bool note = true;  // lit by a note
        bool rgb = true;   // lit by the RGB SysEx
    };
    std::unordered_map<int, std::vector<Lamp>> lamps_;
    std::unordered_map<int, std::vector<std::string>> meters_;  // ch << 8 | cc
    bool clearLamps_ = false;    // dark, for the guest to light
    bool restoreLamps_ = false;  // back to the layout's, once it is gone

    std::vector<unsigned char> pending_;  // a message not yet complete
    unsigned char status_ = 0;            // running status
    std::deque<std::string> log_;
    unsigned sent_ = 0;
    unsigned received_ = 0;
};
