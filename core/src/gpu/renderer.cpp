#include "vita/gpu/renderer.h"

namespace vita {

std::unique_ptr<Renderer> create_renderer() {
    // À remplacer par un VulkanRenderer quand il existera.
    return std::make_unique<NullRenderer>();
}

}  // namespace vita
