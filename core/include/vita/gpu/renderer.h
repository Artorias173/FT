#pragma once

#include <memory>
#include <vector>

#include "vita/common/types.h"

namespace vita {

// Résolution native de l'écran de la PS Vita.
constexpr u32 kVitaScreenWidth = 960;
constexpr u32 kVitaScreenHeight = 544;

// Image à afficher : tampon déclaré par sceDisplaySetFrameBuf.
struct Frame {
    const u8* pixels;  // valide uniquement pendant l'appel à present()
    u32 width;
    u32 height;
    u32 pitch;         // en pixels
    u32 pixel_format;  // 0 : A8B8G8R8 (octets R, G, B, A en mémoire)
};

// Back-end de rendu. Le GPU de la Vita (PowerVR SGX543MP4+) est piloté par
// l'API SceGxm ; on traduira ses commandes et ses shaders vers Vulkan (le
// choix naturel sur Android). En attendant, on affiche le tampon d'image.
class Renderer {
public:
    virtual ~Renderer() = default;

    // native_window : ANativeWindow* sur Android, nullptr en mode sans affichage.
    virtual bool init(void* native_window, u32 width, u32 height) = 0;
    virtual void resize(u32 width, u32 height) = 0;
    // Appelé à chaque synchronisation verticale ; frame vaut nullptr si aucun tampon n'est défini.
    virtual void present(const Frame* frame) = 0;
    virtual void shutdown() = 0;
};

// Rendu sans affichage : compte les images et, sur demande, garde une copie
// de la dernière (tests).
class NullRenderer final : public Renderer {
public:
    explicit NullRenderer(bool capture = false) : capture_(capture) {}
    bool init(void*, u32, u32) override { return true; }
    void resize(u32, u32) override {}
    void present(const Frame* frame) override;
    void shutdown() override {}

    u64 frames() const { return frames_; }
    const std::vector<u32>& last_frame() const { return last_; }  // pixels A8B8G8R8, sans marge de ligne
    u32 last_width() const { return last_width_; }
    u32 last_height() const { return last_height_; }

private:
    bool capture_;
    u64 frames_ = 0;
    std::vector<u32> last_;
    u32 last_width_ = 0, last_height_ = 0;
};

std::unique_ptr<Renderer> create_renderer();

}  // namespace vita
