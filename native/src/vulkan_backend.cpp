#include "emergent/vulkan_backend.hpp"
#include <volk.h>
#include <vk_mem_alloc.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace emergent {
static VkInstance asInstance(void* p){ return reinterpret_cast<VkInstance>(p); }
static VkPhysicalDevice asPhysical(void* p){ return reinterpret_cast<VkPhysicalDevice>(p); }
static VkDevice asDevice(void* p){ return reinterpret_cast<VkDevice>(p); }
static VkQueue asQueue(void* p){ return reinterpret_cast<VkQueue>(p); }
static VkCommandPool asPool(void* p){ return reinterpret_cast<VkCommandPool>(p); }
static VmaAllocator asAllocator(void* p){ return reinterpret_cast<VmaAllocator>(p); }

bool VulkanBackend::initialize(){
    caps_ = {};
    VkResult r = volkInitialize();
    if (r != VK_SUCCESS) { caps_.error = "volkInitialize failed: " + std::to_string(r); return false; }
    caps_.loader = true;
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO,nullptr,"EMERGENT",VK_MAKE_VERSION(2,0,0),"EMERGENT Native",VK_MAKE_VERSION(2,0,0),VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,nullptr,0,&ai,0,nullptr,0,nullptr};
    VkInstance instance = VK_NULL_HANDLE;
    r = vkCreateInstance(&ici,nullptr,&instance);
    if(r != VK_SUCCESS){ caps_.error="vkCreateInstance failed: "+std::to_string(r); return false; }
    volkLoadInstance(instance); instance_=instance; caps_.instance=true;
    uint32_t count=0; vkEnumeratePhysicalDevices(instance,&count,nullptr);
    if(!count){ caps_.error="No Vulkan physical device"; shutdown(); return false; }
    std::vector<VkPhysicalDevice> devices(count); vkEnumeratePhysicalDevices(instance,&count,devices.data());
    VkPhysicalDevice physical=devices.front(); physical_device_=physical; caps_.physical_device=true;
    VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(physical,&props); caps_.device_name=props.deviceName;
    caps_.api_version=std::to_string(VK_VERSION_MAJOR(props.apiVersion))+"."+std::to_string(VK_VERSION_MINOR(props.apiVersion))+"."+std::to_string(VK_VERSION_PATCH(props.apiVersion));
    uint32_t qcount=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&qcount,nullptr); std::vector<VkQueueFamilyProperties> qprops(qcount); vkGetPhysicalDeviceQueueFamilyProperties(physical,&qcount,qprops.data());
    uint32_t family=UINT32_MAX; for(uint32_t i=0;i<qcount;i++) if((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (qprops[i].queueFlags & VK_QUEUE_TRANSFER_BIT)){ family=i; break; }
    if(family==UINT32_MAX){ caps_.error="No graphics+compute+transfer queue family"; shutdown(); return false; }
    queue_family_=family;
    float priority=1.0f; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,nullptr,0,family,1,&priority};
    VkPhysicalDeviceFeatures features{}; VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,nullptr,0,1,&qci,0,nullptr,0,nullptr,&features};
    VkDevice device=VK_NULL_HANDLE; r=vkCreateDevice(physical,&dci,nullptr,&device); if(r!=VK_SUCCESS){caps_.error="vkCreateDevice failed: "+std::to_string(r); shutdown(); return false;}
    volkLoadDevice(device); device_=device; caps_.device=true; vkGetDeviceQueue(device,family,0,reinterpret_cast<VkQueue*>(&queue_)); caps_.queue=true; caps_.compute=true; caps_.transfer=true;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,nullptr,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,family}; VkCommandPool pool=VK_NULL_HANDLE; r=vkCreateCommandPool(device,&pci,nullptr,&pool); if(r!=VK_SUCCESS){caps_.error="vkCreateCommandPool failed: "+std::to_string(r); shutdown(); return false;} command_pool_=pool;
    VmaAllocatorCreateInfo vaci{}; vaci.vulkanApiVersion=VK_API_VERSION_1_3; vaci.physicalDevice=physical; vaci.device=device; vaci.instance=instance;
    // EMERGENT builds VMA with dynamic Vulkan functions, so it holds no static
    // binding of its own. Hand it the dispatch tables volk already resolved;
    // vmaCreateAllocator copies the pointers, so a stack local is sufficient.
    VmaVulkanFunctions vulkanFunctions{};
    r = vmaImportVulkanFunctionsFromVolk(&vaci, &vulkanFunctions);
    if (r != VK_SUCCESS) { caps_.error = "vmaImportVulkanFunctionsFromVolk failed: " + std::to_string(r); shutdown(); return false; }
    vaci.pVulkanFunctions = &vulkanFunctions;
    VmaAllocator allocator=VK_NULL_HANDLE; r=vmaCreateAllocator(&vaci,&allocator); if(r!=VK_SUCCESS){caps_.error="vmaCreateAllocator failed: "+std::to_string(r); shutdown(); return false;} allocator_=allocator;
    caps_.indirect_draw=true;
    initialized_=true; return true;
}

NativeGpuRenderStats VulkanBackend::renderBootstrap(uint32_t width,uint32_t height){
    NativeGpuRenderStats s; s.width=width; s.height=height; auto start=std::chrono::steady_clock::now();
    if(!initialized_) { s.error="native Vulkan backend not initialized"; return s; }
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,nullptr,0,VK_IMAGE_TYPE_2D,VK_FORMAT_R8G8B8A8_UNORM,{width,height,1},1,1,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_SHARING_MODE_EXCLUSIVE,0,nullptr,VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo aci{}; aci.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE; VkImage image=VK_NULL_HANDLE; VmaAllocation allocation=nullptr; VkResult r=vmaCreateImage(asAllocator(allocator_),&ici,&aci,&image,&allocation,nullptr); if(r!=VK_SUCCESS){s.error="vmaCreateImage failed: "+std::to_string(r);return s;}
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,nullptr,asPool(command_pool_),VK_COMMAND_BUFFER_LEVEL_PRIMARY,1}; VkCommandBuffer cmd=VK_NULL_HANDLE; r=vkAllocateCommandBuffers(asDevice(device_),&cai,&cmd); if(r!=VK_SUCCESS){vmaDestroyImage(asAllocator(allocator_),image,allocation);s.error="vkAllocateCommandBuffers failed";return s;}
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,nullptr}; vkBeginCommandBuffer(cmd,&bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,nullptr,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b); VkClearColorValue clear{{0.025f,0.045f,0.09f,1.0f}}; VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; vkCmdClearColorImage(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask=0; b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout=VK_IMAGE_LAYOUT_GENERAL; vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&b); vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,0,nullptr,nullptr,1,&cmd,0,nullptr}; r=vkQueueSubmit(asQueue(queue_),1,&si,VK_NULL_HANDLE); if(r==VK_SUCCESS) r=vkQueueWaitIdle(asQueue(queue_)); vmaDestroyImage(asAllocator(allocator_),image,allocation); if(r!=VK_SUCCESS){s.error="GPU submission failed: "+std::to_string(r);return s;} s.executed=true; s.submit_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(); return s;
}

void VulkanBackend::shutdown(){
    if(allocator_){vmaDestroyAllocator(asAllocator(allocator_)); allocator_=nullptr;}
    if (device_ && command_pool_) vkDestroyCommandPool(asDevice(device_), asPool(command_pool_), nullptr);
    command_pool_ = nullptr;
    if (device_) {
        vkDestroyDevice(asDevice(device_), nullptr);
        device_ = nullptr;
        queue_ = nullptr;
    }
    if (instance_) {
        vkDestroyInstance(asInstance(instance_), nullptr);
        instance_ = nullptr;
    }
    physical_device_ = nullptr;
    initialized_ = false;
    caps_ = {};
}
} // namespace emergent
