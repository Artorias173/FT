#pragma once
// Rendu Android : copie le tampon d'image de la Vita dans la fenêtre native.
// Solution d'attente avant un rendu Vulkan des commandes SceGxm.

#include <android/native_window.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "vita/gpu/renderer.h"

class AndroidRenderer final : public vita::Renderer {
public:
    ~AndroidRenderer() override { shutdown(); }

    bool init(void* native_window, vita::u32, vita::u32) override {
        std::lock_guard<std::mutex> lock(mutex_);
        release_locked();
        window_ = static_cast<ANativeWindow*>(native_window);
        if (window_) ANativeWindow_acquire(window_);
        width_ = height_ = 0;
        return true;
    }

    void resize(vita::u32, vita::u32) override {}

    void present(const vita::Frame* frame) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!window_ || !frame || frame->pixel_format != 0) return;
        if (frame->width != width_ || frame->height != height_) {
            // Le système met l'image à l'échelle de la surface. RGBX : l'alpha est ignoré.
            ANativeWindow_setBuffersGeometry(window_, static_cast<int32_t>(frame->width),
                                             static_cast<int32_t>(frame->height), WINDOW_FORMAT_RGBX_8888);
            width_ = frame->width;
            height_ = frame->height;
        }
        ANativeWindow_Buffer buffer;
        if (ANativeWindow_lock(window_, &buffer, nullptr) != 0) return;
        auto* dst = static_cast<vita::u8*>(buffer.bits);
        const vita::u32 rows = std::min<vita::u32>(frame->height, static_cast<vita::u32>(buffer.height));
        const vita::u32 cols = std::min<vita::u32>(frame->width, static_cast<vita::u32>(buffer.width));
        for (vita::u32 y = 0; y < rows; ++y)
            std::memcpy(dst + static_cast<size_t>(y) * buffer.stride * 4,
                        frame->pixels + static_cast<size_t>(y) * frame->pitch * 4, cols * 4);
        ANativeWindow_unlockAndPost(window_);
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lock(mutex_);
        release_locked();
    }

private:
    void release_locked() {
        if (window_) ANativeWindow_release(window_);
        window_ = nullptr;
    }

    std::mutex mutex_;
    ANativeWindow* window_ = nullptr;
    vita::u32 width_ = 0, height_ = 0;
};
