#include "Settings.hpp"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>

using nlohmann::json;

std::string settingsPath() {
    char* dir = SDL_GetPrefPath("QEngine", "QEngine");
    if (!dir) return {};

    const std::string path = std::string(dir) + "settings.json";
    SDL_free(dir);
    return path;
}

Settings Settings::load(const std::string& path) {
    Settings settings;
    if (path.empty()) return settings;

    size_t size = 0;
    const std::unique_ptr<char, decltype(&SDL_free)> file(
        static_cast<char*>(SDL_LoadFile(path.c_str(), &size)), SDL_free);
    if (!file) return settings;

    const json data = json::parse(file.get(), file.get() + size, nullptr, false);
    if (!data.is_object()) return settings;

    if (auto value = data.find("jogSensitivity"); value != data.end() && value->is_number()) {
        settings.jogSensitivity = float(std::clamp(value->get<double>(), double(minJogSensitivity),
                                                  double(maxJogSensitivity)));
    }

    if (auto value = data.find("haptics"); value != data.end() && value->is_boolean()) {
        settings.haptics = value->get<bool>();
    }

    return settings;
}

bool Settings::save(const std::string& path) const {
    if (path.empty()) return false;

    const json data = {{"jogSensitivity", jogSensitivity}, {"haptics", haptics}};
    const std::string contents = data.dump(2) + '\n';
    const std::string temp = path + "." + std::to_string(SDL_GetPerformanceCounter()) + ".tmp";
    SDL_IOStream* file = SDL_IOFromFile(temp.c_str(), "wx");
    if (!file) return false;

    // Keep the previous preferences until the new file has been written and closed.
    if (SDL_SaveFile_IO(file, contents.data(), contents.size(), true) && SDL_RenamePath(temp.c_str(), path.c_str())) {
        return true;
    }

    SDL_RemovePath(temp.c_str());
    return false;
}
