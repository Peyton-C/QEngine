#include "Machines.hpp"
#include "Catalog.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

extern char** environ;

namespace {

// Single quoted for bash, for the command shown to the reader and the
// wrapper script.
std::string shq(const std::string& s) {
    if (!s.empty() && s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.,/:=+@") ==
                          std::string::npos) {
        return s;
    }

    std::string out = "'";

    for (char c : s) {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }

    return out + "'";
}

std::unordered_map<std::string, std::string> readEnv(const fs::path& file) {
    std::unordered_map<std::string, std::string> v;
    std::ifstream in(file);
    std::string line;

    while (std::getline(in, line)) {
        const size_t eq = line.find('=');

        if (eq == std::string::npos || line.empty() || line[0] == '#') continue;

        std::string value = line.substr(eq + 1);

        // AUDIO_CARD_NAME is written quoted, since a card name has spaces.
        if (value.size() >= 2 && (value.front() == '\'' || value.front() == '"') && value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }

        v[line.substr(0, eq)] = value;
    }

    return v;
}

int toInt(const std::string& s, int fallback = 0) {
    try {
        return s.empty() ? fallback : std::stoi(s);
    } catch (const std::exception&) {
        return fallback;
    }
}

struct Process {
    int pid = 0;
    std::string command;
};

// Every process and its full command line. ps rather than a platform API,
// since its output is the same on macOS and Linux.
std::vector<Process> processes() {
    std::vector<Process> out;
    FILE* ps = popen("ps -axww -o pid= -o command= 2>/dev/null", "r");

    if (!ps) return out;

    std::string line;
    char buf[4096];

    while (fgets(buf, sizeof buf, ps)) {
        line += buf;

        if (line.empty() || line.back() != '\n') continue;

        line.pop_back();
        const size_t start = line.find_first_not_of(' ');
        const size_t space = line.find(' ', start);

        if (start != std::string::npos && space != std::string::npos) {
            out.push_back({toInt(line.substr(start, space - start)), line.substr(space + 1)});
        }

        line.clear();
    }

    pclose(ps);
    return out;
}

bool hasArg(const std::string& command, const std::string& arg) {
    for (size_t at = command.find(arg); at != std::string::npos; at = command.find(arg, at + 1)) {
        const size_t end = at + arg.size();

        if (end == command.size() || command[end] == ' ') return true;
    }

    return false;
}

}  // namespace

Machines::Machines(fs::path repo, fs::path instances) : repo_(std::move(repo)), instances_(std::move(instances)) {
    list_ = scan();
    thread_ = std::thread([this] { loop(); });
}

Machines::~Machines() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

std::vector<Instance> Machines::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return list_;
}

void Machines::rescan() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rescan_ = true;
    }
    wake_.notify_all();
}

void Machines::loop() {
    while (!quit_) {
        reap();
        std::vector<Instance> fresh = scan();

        std::unique_lock<std::mutex> lock(mutex_);
        list_ = std::move(fresh);
        wake_.wait_for(lock, std::chrono::milliseconds(1500), [this] { return quit_ || rescan_; });
        rescan_ = false;
    }
}

std::vector<Instance> Machines::scan() {
    std::vector<Instance> found;
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(instances_, ec)) {
        if (!entry.is_directory(ec)) continue;

        const fs::path env = entry.path() / "instance.env";
        const bool haveEnv = fs::exists(env, ec);
        const bool haveLog = fs::exists(entry.path() / "build.log", ec);

        // Something else's directory, not an instance or a build of one.
        if (!haveEnv && !haveLog) continue;

        Instance i;
        i.name = entry.path().filename().string();
        i.dir = entry.path();

        // A failed --force rebuild leaves instance.env behind but takes the
        // stamp away. An instance from before the stamp has neither it nor
        // a build log from here.
        i.ready = haveEnv && (fs::exists(i.dir / ".rootfs.complete", ec) || !haveLog);

        if (!haveEnv) {
            auto v = readEnv(i.dir / ".build-request");
            i.product = v["PRODUCT_CODE"];
            i.firmware = v["FIRMWARE_IMG"];
            i.montero = v["MONTERO"] == "1";
        } else {
            auto v = readEnv(env);
            i.device = v["DEVICE"];
            i.arch = v["ARCH"];
            i.product = v["PRODUCT_CODE"];
            i.firmware = v["FIRMWARE_IMG"];
            i.display = v["DISPLAY_MODE"];
            i.montero = v["MONTERO"] == "1";
            i.rootfs = v["ROOTFS_IMG"];
            i.sshPort = toInt(v["SSH_PORT"], 2225);

            // An arm64 HeadRush guest runs VX5, even in old instances built
            // with HG04. The armv7 updaters each carry their own app and can
            // be built from the CLI without recording a product code.
            if (i.device == "headrush" && i.arch == "arm64") {
                i.product = "HV03";
            }

            // A builder's default, when the instance was built without
            // --product-code.
            if (i.product.empty()) {
                if (i.device == "mpc") i.product = i.arch == "arm64" ? "ACVA3" : "ACV5";
                else if (i.device == "mpc-sample") i.product = "AC50";
                else if (i.device == "drummodule") i.product = "LDMD";
                else if (i.device == "engine") i.product = i.arch == "arm64" ? "RMZ2" : "JP07";
                else if (i.device == "headrush") {
                    if (const Product* p = findHeadRushFirmware(fs::path(i.firmware).filename().string())) {
                        i.product = p->code;
                    }
                }
            }

            // The families whose guests serve a panel over MIDI: Engine
            // through midibridge, MPC and MPC Sample through mpcsurface. One
            // from before MIDI_PORT was recorded is served where
            // run_instance.sh puts it, 4000 above its SSH port.
            if (i.device == "engine" || i.device == "mpc" || i.device == "mpc-sample") {
                i.port = toInt(v["MIDI_PORT"], i.sshPort + 4000);
            }

            if (!v["ENGINE_VNC_PORT"].empty()) {
                i.screenPort = toInt(v["ENGINE_VNC_PORT"]);
                i.engineVnc = true;
            } else {
                // run_qemu.sh's default when none is recorded.
                i.screenPort = 5900 + toInt(v["VNC_DISPLAY"], 1);
            }
        }

        found.push_back(std::move(i));
    }

    for (const Process& p : processes()) {
        const bool qemu = p.command.find("qemu-system") != std::string::npos;
        const bool build = p.command.find("new_instance.sh") != std::string::npos;

        if (!qemu && !build) continue;

        for (Instance& i : found) {
            if (qemu && !i.rootfs.empty() && p.command.find(i.rootfs) != std::string::npos) {
                i.qemuPid = p.pid;
            }

            if (build && hasArg(p.command, "--name " + i.name) && (!i.buildPid || p.pid < i.buildPid)) {
                i.buildPid = p.pid;
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);

    for (Instance& i : found) {
        if (i.buildPid) {
            i.state = MachineState::Building;
        } else if (!i.ready) {
            i.state = MachineState::Failed;
        } else if (i.qemuPid) {
            i.state = stopping_.count(i.name) ? MachineState::Stopping : MachineState::Running;
        } else {
            stopping_.erase(i.name);
        }
    }

    std::sort(found.begin(), found.end(), [](const Instance& a, const Instance& b) { return a.name < b.name; });
    return found;
}

// Runs argv in its own session, from the repo, with stdout and stderr in
// `log`, so it neither dies with the UI nor takes a terminal's ^C meant for
// it. Returns the pid, or 0.
int Machines::spawn(const std::vector<std::string>& argv, const fs::path& log) {
    std::vector<char*> args;

    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));

    args.push_back(nullptr);
    const std::string logPath = log.string();
    const std::string cwd = repo_.string();
    const long maxFd = std::min(sysconf(_SC_OPEN_MAX), 65536L);

    const pid_t pid = fork();

    if (pid == 0) {
        // Only async-signal-safe calls from here, since the UI has threads.
        setsid();

        if (chdir(cwd.c_str()) != 0) _exit(127);

        const int out = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const int in = open("/dev/null", O_RDONLY);

        if (out < 0 || in < 0) _exit(127);

        dup2(in, 0);
        dup2(out, 1);
        dup2(out, 2);

        // Nothing of the UI's, such as its VNC socket, which would keep the
        // guest's end open after the UI closes it.
        for (long fd = 3; fd < maxFd; ++fd) close(int(fd));

        execve(args[0], args.data(), environ);
        _exit(127);
    }

    if (pid < 0) return 0;

    std::lock_guard<std::mutex> lock(mutex_);
    children_.push_back(pid);
    return pid;
}

void Machines::reap() {
    std::lock_guard<std::mutex> lock(mutex_);

    children_.erase(std::remove_if(children_.begin(), children_.end(),
                                   [](int pid) {
                                       int status = 0;
                                       return waitpid(pid, &status, WNOHANG) != 0;
                                   }),
                    children_.end());
}

bool Machines::start(const Instance& i, std::string& error) {
    if (!i.ready || i.state != MachineState::Stopped) {
        error = i.name + " is not stopped.";
        return false;
    }

    // The panel reads the screen over VNC, so a display that would open a
    // window of its own is swapped for its VNC twin, GL for GL.
    std::string display = i.display;

    if (display == "sdl-gl") display = "egl-vnc";
    else if (display == "sdl" || display == "cocoa" || display.empty()) display = "vnc";

    const std::vector<std::string> argv = {"/bin/bash", (repo_ / "scripts/qemu/run_instance.sh").string(),
                                           "--name", i.name, "--display", display};

    if (!spawn(argv, consoleLog(i))) {
        error = std::string("fork: ") + std::strerror(errno);
        return false;
    }

    rescan();
    return true;
}

void Machines::stop(const Instance& i, bool force) {
    if (i.qemuPid <= 0) return;

    kill(i.qemuPid, force ? SIGKILL : SIGTERM);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_[i.name] = 0;
    }
    rescan();
}

std::string Machines::command(const BuildRequest& r) const {
    std::string firmware = r.firmware;
    const std::string root = repo_.string() + "/";

    if (firmware.compare(0, root.size(), root) == 0) firmware = firmware.substr(root.size());

    std::string c = "scripts/build_scripts/new_instance.sh --name " + shq(r.name) + " --firmware " + shq(firmware);

    if (!r.product.empty()) c += " --product-code " + shq(r.product);
    if (r.montero) c += " --montero";
    else if (r.force) c += " --no-montero";
    if (r.force) c += " --force";
    return c;
}

bool Machines::build(const BuildRequest& r, std::string& error) {
    if (r.name.empty() || r.name.find('/') != std::string::npos || r.name[0] == '.') {
        error = "That name cannot be a directory.";
        return false;
    }

    const fs::path dir = instances_ / r.name;
    std::error_code ec;
    fs::create_directories(dir, ec);

    if (ec) {
        error = ec.message();
        return false;
    }

    // What was asked for, so a build that fails before writing
    // instance.env can still be listed by product and tried again.
    {
        std::ofstream req(dir / ".build-request");
        req << "PRODUCT_CODE=" << r.product << "\nFIRMWARE_IMG=" << r.firmware << "\nMONTERO=" << (r.montero ? 1 : 0)
            << "\n";
    }

    // Settings new_instance.sh does not write, added once it has, unless
    // instance.env already sets them.
    std::string script = "\"$0\" \"$@\" || exit $?\n";

    if (!r.extraEnv.empty()) {
        const std::string env = shq((dir / "instance.env").string());
        script += "for kv in";

        for (const std::string& kv : r.extraEnv) script += " " + shq(kv);

        script += "; do grep -q \"^${kv%%=*}=\" " + env + " || printf '%s\\n' \"$kv\" >> " + env + "; done\n";
    }

    std::vector<std::string> argv = {"/bin/bash", "-c", script,
                                     (repo_ / "scripts/build_scripts/new_instance.sh").string(),
                                     "--name", r.name, "--firmware", r.firmware};

    if (!r.product.empty()) {
        argv.push_back("--product-code");
        argv.push_back(r.product);
    }

    argv.push_back(r.montero ? "--montero" : "--no-montero");

    if (r.force) argv.push_back("--force");

    if (!spawn(argv, dir / "build.log")) {
        error = std::string("fork: ") + std::strerror(errno);
        return false;
    }

    // Listed as building straight away, rather than at the next scan, so
    // the sheet can select it as it closes.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(list_.begin(), list_.end(), [&](const Instance& i) { return i.name == r.name; });

        if (it == list_.end()) {
            Instance i;
            i.name = r.name;
            i.dir = dir;
            i.product = r.product;
            i.firmware = r.firmware;
            it = list_.insert(std::upper_bound(list_.begin(), list_.end(), i,
                                               [](const Instance& a, const Instance& b) { return a.name < b.name; }),
                              i);
        }

        it->state = MachineState::Building;
    }

    rescan();
    return true;
}

void Machines::cancelBuild(const Instance& i) {
    if (i.buildPid <= 0) return;

    // The whole session spawn() made, so the builder and its docker client
    // go too; one started from a terminal is only sent its own.
    const pid_t group = getpgid(i.buildPid);

    if (group > 0 && group != getpgrp()) kill(-group, SIGTERM);
    else kill(i.buildPid, SIGTERM);

    rescan();
}

bool Machines::remove(const Instance& i, std::string& error) {
    if (i.state == MachineState::Running || i.state == MachineState::Stopping ||
        i.state == MachineState::Building) {
        error = "Stop " + i.name + " first.";
        return false;
    }

    // Only ever a directory directly under build/instances.
    if (i.name.empty() || i.dir.parent_path() != instances_ || i.dir.filename() != i.name) {
        error = "Not an instance directory: " + i.dir.string();
        return false;
    }

    std::error_code ec;
    fs::remove_all(i.dir, ec);

    if (ec) {
        error = ec.message();
        return false;
    }

    rescan();
    return true;
}

const std::vector<std::string>& LogTail::read(const fs::path& path, size_t lines) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);

    if (ec) {
        path_ = path;
        size_ = ~uintmax_t(0);
        lines_.clear();
        return lines_;
    }

    if (path == path_ && size == size_) return lines_;

    path_ = path;
    size_ = size;
    lines_.clear();

    std::ifstream in(path, std::ios::binary);
    const uintmax_t window = 256 * 1024;

    if (size > window) in.seekg(std::streamoff(size - window));

    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t at = 0;

    // Mid-line when the window cut into one.
    if (size > window) {
        at = text.find('\n');
        at = at == std::string::npos ? text.size() : at + 1;
    }

    while (at < text.size()) {
        size_t end = text.find('\n', at);

        if (end == std::string::npos) end = text.size();

        std::string raw = text.substr(at, end - at);
        at = end + 1;

        while (!raw.empty() && raw.back() == '\r') raw.pop_back();

        const size_t cr = raw.rfind('\r');

        if (cr != std::string::npos) raw = raw.substr(cr + 1);

        std::string line;

        for (size_t k = 0; k < raw.size(); ++k) {
            if (raw[k] == '\x1b' && k + 1 < raw.size() && raw[k + 1] == '[') {
                k += 2;

                while (k < raw.size() && !std::isalpha((unsigned char)raw[k])) ++k;
            } else if (raw[k] == '\t') {
                line += "    ";
            } else if ((unsigned char)raw[k] >= 0x20) {
                line += raw[k];
            }
        }

        lines_.push_back(std::move(line));
    }

    if (lines_.size() > lines) lines_.erase(lines_.begin(), lines_.end() - long(lines));

    return lines_;
}
