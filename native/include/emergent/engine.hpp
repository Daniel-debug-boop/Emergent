#pragma once
#include "emergent/vulkan_backend.hpp"
#include "emergent/jolt_physics.hpp"
#include "emergent/optimization.hpp"
#include "emergent/navigation.hpp"
#include "emergent/ecs_world.hpp"
#include "emergent/audio.hpp"
#include <cstdint>
#include <vector>
#include <string>
namespace emergent {
struct ObjectGPU { float x,y,z,radius; uint32_t material; uint32_t lod; float importance; float pad; };
class NativeEngine { public: bool initialize(); int runSelfTest(); NativeGpuRenderStats renderBootstrap(uint32_t width=256,uint32_t height=256); void shutdown(); const VulkanCapabilities& vulkan() const{return vk_.capabilities();} private: VulkanBackend vk_; JoltPhysicsWorld physics_; EcsWorld ecs_; AudioSystem audio_; };
}
