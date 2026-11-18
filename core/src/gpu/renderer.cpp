#include "vita/gpu/renderer.h"

#include <cstring>

namespace vita {

void NullRenderer::present(const Frame* frame) {
    if (!frame) return;
    ++frames_;
    if (!capture_) return;
    last_width_ = frame->width;
    last_height_ = frame->height;
    last_.resize(static_cast<size_t>(frame->width) * frame->height);
    for (u32 y = 0; y < frame->height; ++y)
        std::memcpy(&last_[static_cast<size_t>(y) * frame->width], frame->pixels + static_cast<size_t>(y) * frame->pitch * 4,
                    frame->width * 4);
}

std::unique_ptr<Renderer> create_renderer() {
    // L'application Android fournit son propre rendu (fenêtre native).
    return std::make_unique<NullRenderer>();
}

}  // namespace vita
