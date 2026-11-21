#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The instances under build/instances, the way a VM manager lists its
// machines, and the scripts that build, start and stop them.
//
// Nothing here owns a VM. new_instance.sh and run_instance.sh run detached,
// in their own session with their output in a log in the instance's
// directory, so they outlive the UI, and a VM started from a terminal is
// listed as running the same as one started here. What is running is read
// from the process table: a QEMU whose command line names the instance's
// rootfs, and a new_instance.sh given its name.

enum class MachineState {
    Stopped,
    Running,
    Stopping,  // asked to stop, not gone yet
    Building,
    Failed,  // a build that did not finish, so there is no complete rootfs
};

// One directory under build/instances. A directory a build has started in
// but not finished has no instance.env yet, and only its name is known.
struct Instance {
    std::string name;
    std::filesystem::path dir;
    bool ready = false;  // instance.env and a finished rootfs

    std::string device;   // DEVICE: engine, mpc, mpc-sample, headrush, drummodule
    std::string arch;     // ARCH: arm64 or armhf
    std::string product;  // PRODUCT_CODE, or the builder's default
    std::string firmware; // FIRMWARE_IMG, as recorded
    std::string display;  // DISPLAY_MODE, empty when the host picks it
    bool montero = false;
    std::string rootfs;   // ROOTFS_IMG

    int sshPort = 0;
    int port = 0;          // MIDI, for the families whose guests serve a panel
    int screenPort = 0;    // VNC
    bool engineVnc = false;  // the screen is Engine's own Qt VNC server

    MachineState state = MachineState::Stopped;
    int qemuPid = 0;
    int buildPid = 0;
};

// What a new instance, or a rebuild of one, is built from.
struct BuildRequest {
    std::string name;
    std::string firmware;
    std::string product;
    bool montero = false;
    bool force = false;  // rebuild an existing instance's rootfs
    // KEY=VALUE lines added to instance.env when the build succeeds, when it
    // does not set the key itself, as JP22's GPU_MAX_OUTPUTS=3.
    std::vector<std::string> extraEnv;
};

class Machines {
public:
    Machines(std::filesystem::path repo, std::filesystem::path instances);
    ~Machines();

    // A copy of the last scan, sorted by name.
    std::vector<Instance> list() const;
    // Scan again now rather than at the next tick, after starting something.
    void rescan();

    bool start(const Instance& i, std::string& error);
    // SIGTERM, which QEMU takes as a clean quit; `force` is SIGKILL.
    void stop(const Instance& i, bool force = false);
    bool build(const BuildRequest& r, std::string& error);
    void cancelBuild(const Instance& i);
    bool remove(const Instance& i, std::string& error);

    std::string command(const BuildRequest& r) const;  // as typed into a shell
    std::filesystem::path consoleLog(const Instance& i) const { return i.dir / "console.log"; }
    std::filesystem::path buildLog(const Instance& i) const { return i.dir / "build.log"; }
    const std::filesystem::path& instancesDir() const { return instances_; }
    const std::filesystem::path& repo() const { return repo_; }

private:
    void loop();
    std::vector<Instance> scan();
    int spawn(const std::vector<std::string>& argv, const std::filesystem::path& log);
    void reap();

    std::filesystem::path repo_;
    std::filesystem::path instances_;

    mutable std::mutex mutex_;
    std::vector<Instance> list_;
    std::map<std::string, double> stopping_;  // name, when asked
    std::vector<int> children_;               // to reap

    std::thread thread_;
    std::condition_variable wake_;
    std::atomic<bool> quit_{false};
    bool rescan_ = false;
};

// The last lines of a log, read again only when it grows. Progress bars that
// redraw with a carriage return keep only their last state, and terminal
// colour codes are dropped.
class LogTail {
public:
    const std::vector<std::string>& read(const std::filesystem::path& path, size_t lines = 400);

private:
    std::filesystem::path path_;
    uintmax_t size_ = ~uintmax_t(0);
    std::vector<std::string> lines_;
};
