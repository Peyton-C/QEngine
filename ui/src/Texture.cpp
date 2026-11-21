#include "Texture.hpp"

#include <algorithm>
#include <cstring>

Texture::~Texture() {
    reset();
}

void Texture::reset() {
    if (texture_) {
        SDL_ReleaseGPUTexture(device_, texture_);
    }

    texture_ = nullptr;
    width_ = 0;
    height_ = 0;
}

bool Texture::load(SDL_GPUDevice* device, const std::string& path) {
    reset();

    SDL_Surface* loaded = SDL_LoadPNG(path.c_str());

    if (!loaded) {
        return false;
    }

    // RGBA byte order, to match R8G8B8A8 below whatever the PNG held.
    SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);

    if (!rgba) {
        return false;
    }

    const Uint32 w = Uint32(rgba->w);
    const Uint32 h = Uint32(rgba->h);
    Uint32 levels = 1;

    while ((std::max(w, h) >> levels) > 0) {
        ++levels;
    }

    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    // Mip generation renders into each level, so it needs COLOR_TARGET too.
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = 1;
    info.num_levels = levels;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);

    SDL_GPUTransferBufferCreateInfo tb = {};
    tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tb.size = w * h * 4;
    SDL_GPUTransferBuffer* transfer = texture ? SDL_CreateGPUTransferBuffer(device, &tb) : nullptr;

    if (!transfer) {
        if (texture) {
            SDL_ReleaseGPUTexture(device, texture);
        }

        SDL_DestroySurface(rgba);
        return false;
    }

    // The surface's rows may be padded, the transfer buffer's are not.
    auto* dst = static_cast<Uint8*>(SDL_MapGPUTransferBuffer(device, transfer, false));

    for (Uint32 y = 0; y < h; ++y) {
        std::memcpy(dst + y * w * 4, static_cast<Uint8*>(rgba->pixels) + y * rgba->pitch, w * 4);
    }

    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_DestroySurface(rgba);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = transfer;

    SDL_GPUTextureRegion region = {};
    region.texture = texture;
    region.w = w;
    region.h = h;
    region.d = 1;

    SDL_UploadToGPUTexture(copy, &src, &region, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GenerateMipmapsForGPUTexture(cmd, texture);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device, transfer);

    device_ = device;
    texture_ = texture;
    width_ = int(w);
    height_ = int(h);
    return true;
}
