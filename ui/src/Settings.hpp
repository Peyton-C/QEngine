#pragma once

#include <string>

struct Settings {
    static constexpr float minJogSensitivity = 0.25f;
    static constexpr float maxJogSensitivity = 4.0f;

    float jogSensitivity = 1.0f;
    bool haptics = true;

    static Settings load(const std::string& path);
    bool save(const std::string& path) const;
};

std::string settingsPath();
