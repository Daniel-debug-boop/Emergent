// Vulkan Memory Allocator — implementation translation unit.
//
// VMA upstream ships as a header-only INTERFACE target: the CMake target
// `GPUOpen::VulkanMemoryAllocator` carries only include paths. The allocator's
// symbols (vmaCreateAllocator, vmaCreateImage, vmaDestroyImage, ...) are emitted
// only by a translation unit that defines VMA_IMPLEMENTATION, and it must be
// defined in exactly one place or the link fails with undefined references.
//
// `volk.h` is included first on purpose. VMA detects VOLK_HEADER_VERSION and,
// with VMA_DYNAMIC_VULKAN_FUNCTIONS enabled, routes its Vulkan calls through
// the dispatch tables that volk already loads, instead of a second static
// binding to the Vulkan loader. vulkan_backend.cpp imports those tables with
// vmaImportVulkanFunctionsFromVolk() before creating the allocator.

#define VMA_IMPLEMENTATION
#include <volk.h>
#include <vk_mem_alloc.h>
