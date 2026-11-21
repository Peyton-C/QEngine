#pragma once

#include <SDL3/SDL.h>

#include "imgui.h"

#include <string>

// One mipmapped GPU texture loaded from a PNG, for --overlay. Product photos
// are drawn far smaller than they are stored (the MPC One's is 2852 px wide),
// so without mips they shimmer; the ImGui backend's sampler already reads them.
class Texture {
public:
    Texture() = default;
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    bool load(SDL_GPUDevice* device, const std::string& path);
    void reset();

    bool valid() const { return texture_ != nullptr; }
    ImVec2 size() const { return {float(width_), float(height_)}; }
    ImTextureID id() const { return (ImTextureID)(intptr_t)texture_; }

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* texture_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};
