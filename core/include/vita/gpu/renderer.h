#pragma once

#include <memory>

#include "vita/common/types.h"

namespace vita {

// Résolution native de l'écran de la PS Vita.
constexpr u32 kVitaScreenWidth = 960;
constexpr u32 kVitaScreenHeight = 544;

// Back-end de rendu. Le GPU de la Vita (PowerVR SGX543MP4+) est piloté par
// l'API SceGxm ; on traduira ses commandes et ses shaders vers Vulkan (le
// choix naturel sur Android).
class Renderer {
public:
    virtual ~Renderer() = default;

    // native_window : ANativeWindow* sur Android, nullptr en mode sans affichage.
    virtual bool init(void* native_window, u32 width, u32 height) = 0;
    virtual void resize(u32 width, u32 height) = 0;
    virtual void present() = 0;
    virtual void shutdown() = 0;
};

// Rendu factice : utile pour les tests et tant que Vulkan n'est pas branché.
class NullRenderer final : public Renderer {
public:
    bool init(void*, u32, u32) override { return true; }
    void resize(u32, u32) override {}
    void present() override { ++frames_; }
    void shutdown() override {}
    u64 frames() const { return frames_; }

private:
    u64 frames_ = 0;
};

std::unique_ptr<Renderer> create_renderer();

}  // namespace vita
