#include "Catalog.hpp"

#include <cctype>
#include <regex>

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

}  // namespace

const std::vector<Product>& catalog() {
    static const std::vector<Product> products = {
        {"RMZ2", "RANE SYSTEM ONE", "Engine", "arm64", {"SYSTEMONE"}, true},
        {"JP24", "Denon DJ Prime 4 G2", "Engine", "arm64", {"PRIME4G2", "Prime 4 G2"}, true},
        {"JP22", "JP22", "Engine", "arm64", {}, true},
        {"JC11", "Denon DJ Prime 4", "Engine", "armv7", {"PRIME4"}, true},
        {"JC11S", "Denon DJ Prime 4+", "Engine", "armv7", {"PRIME4PLUS"}, true},
        {"JC16", "Denon DJ Prime 2", "Engine", "armv7", {"PRIME2"}, true},
        {"JP11", "Denon DJ Prime GO", "Engine", "armv7", {"PRIMEGO", "Prime GO"}, true},
        {"JP11S", "Denon DJ Prime GO+", "Engine", "armv7", {"PRIMEGOPLUS"}, true},
        {"JP07", "Denon DJ SC5000 Prime", "Engine", "armv7", {"SC5000"}, true},
        {"JP08", "Denon DJ SC5000M Prime", "Engine", "armv7", {"SC5000M"}, true},
        {"JP13", "Denon DJ SC6000 Prime", "Engine", "armv7", {"SC6000"}, true},
        {"JP14", "Denon DJ SC6000M Prime", "Engine", "armv7", {"SC6000M"}, true},
        {"JP20", "Denon DJ SC Live 2", "Engine", "armv7", {"SCLIVE2"}, true},
        {"JP21", "Denon DJ SC Live 4", "Engine", "armv7", {"SCLIVE4"}, true},
        {"NH08", "Numark Mixstream Pro", "Engine", "armv7", {"MIXSTREAMPRO", "MixstreamPro"}, true},
        {"NH08S", "Numark Mixstream Pro+", "Engine", "armv7", {"MIXSTREAMPROPLUS"}, true},
        {"NH10", "Numark Mixstream Pro GO", "Engine", "armv7", {"MIXSTREAMPROGO"}, true},

        {"ACVA3", "MPC One G2", "MPC", "arm64", {"MPC-"}},
        {"ACVG", "MPC Live III", "MPC", "arm64", {"MPC-"}},
        {"ACVC", "MPC XL", "MPC", "arm64", {"MPC-"}},
        {"ACVM2", "MPC Key 61 (Gen 2)", "MPC", "arm64", {"MPC-"}},
        {"ACVR2", "MPC Key 37 G2", "MPC", "arm64", {"MPC-"}},
        {"ADA2S", "Force (Gen 2)", "MPC", "arm64", {"Force-", "force-"}},
        {"ACVA", "MPC One", "MPC", "armv7", {"MPC-"}},
        {"ACVA2", "MPC One+", "MPC", "armv7", {"MPC-"}},
        {"ACV5", "MPC X", "MPC", "armv7", {"MPC-"}},
        {"ACV5S", "MPC X SE", "MPC", "armv7", {"MPC-"}},
        {"ACV8", "MPC Live", "MPC", "armv7", {"MPC-"}},
        {"ACVB", "MPC Live Mk 2", "MPC", "armv7", {"MPC-"}},
        {"ACVM", "MPC Key 61", "MPC", "armv7", {"MPC-"}},
        {"ACVR", "MPC Key 37", "MPC", "armv7", {"MPC-"}},
        {"ADA2", "Force", "MPC", "armv7", {"Force-", "force-"}},
        {"AC50", "Akai MPC Sample", "MPC", "arm64", {"MPC Sample"}},

        {"HV03", "HeadRush VX5", "HeadRush", "arm64", {"HeadRush-VX5"}},
        {"HG06", "HeadRush Prime", "HeadRush", "armv7", {"HeadRush Prime"}},
        {"HG12", "HeadRush Flex Prime", "HeadRush", "armv7", {"HeadRush Flex Prime"}},
        {"HV01", "HeadRush Core", "HeadRush", "armv7", {"HeadRush Core"}},
        {"HG08", "Sheeran Looper X", "HeadRush", "armv7", {"Sheeran Looper X"}},
        {"MG01", "HeadRush Pedalboard", "HeadRush", "armv7", {"HeadRush Pedalboard"}},
        {"HG02", "HeadRush Gigboard", "HeadRush", "armv7", {"HeadRush Gigboard"}},
        {"HG04", "HeadRush MX5", "HeadRush", "armv7", {"HeadRush MX5"}},
        {"HG03", "HeadRush Looperboard", "HeadRush", "armv7", {"HeadRush Looperboard"}},

        {"LDMD", "Alesis Prime Drum Module", "Alesis", "armv7", {"drummodule"}},
        {"LDMF", "Alesis Core Drum Module", "Alesis", "armv7", {"drummodule"}},
    };
    return products;
}

const Product* findProduct(const std::string& code) {
    for (const Product& p : catalog()) {
        if (p.code == code) return &p;
    }
    return nullptr;
}

const Product* findHeadRushFirmware(const std::string& filename) {
    std::string name = lower(filename);

    // new_instance.sh writes the path with printf %q, so a space in an
    // updater's name can arrive as a backslash followed by a space.
    for (size_t pos = name.find("\\ "); pos != std::string::npos; pos = name.find("\\ ", pos + 1)) {
        name.erase(pos, 1);
    }

    for (const Product& p : catalog()) {
        if (p.family != "HeadRush") continue;

        for (const std::string& prefix : p.images) {
            if (name.compare(0, prefix.size(), lower(prefix)) == 0) return &p;
        }
    }

    return nullptr;
}

std::string firmwareVersion(const std::string& filename) {
    static const std::regex separated(R"((?:^|[-_ v])(\d+(?:[._]\d+){1,3}))");
    static const std::regex any(R"((\d+(?:[._]\d+){1,3}))");
    static const std::regex beta(R"([-. ]?beta[-. ]?(\d+))", std::regex::icase);

    // A product's own prefix first, so PRIME41.6.0BETA2 is PRIME4's 1.6.0
    // and not 41.6.0.
    std::string rest = filename;
    size_t longest = 0;

    for (const Product& p : catalog()) {
        for (const std::string& prefix : p.images) {
            if (prefix.size() > longest && lower(filename).compare(0, prefix.size(), lower(prefix)) == 0) {
                longest = prefix.size();
            }
        }
    }

    rest = filename.substr(longest);
    std::smatch m;
    std::string version;

    if (std::regex_search(rest, m, separated) || std::regex_search(rest, m, any)) {
        version = m[1];
    }

    for (char& c : version) {
        if (c == '_') c = '.';
    }

    if (!version.empty() && std::regex_search(rest, m, beta)) {
        version += " beta " + m[1].str();
    }

    return version;
}

std::string defaultInstanceName(const std::string& code, const std::string& firmware) {
    std::string version = firmwareVersion(firmware);
    const size_t beta = version.find(" beta ");

    if (beta != std::string::npos) {
        version = version.substr(0, beta) + "b" + version.substr(beta + 6);
    }

    std::string name = lower(code);
    return version.empty() ? name : name + "-" + version;
}
