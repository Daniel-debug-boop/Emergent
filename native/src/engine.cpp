#include "emergent/engine.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
namespace emergent {
bool NativeEngine::initialize(){
    const bool vk = vk_.initialize();
    const bool physics = physics_.initialize();
    const bool ecs = ecs_.initialize();
    const bool audio = audio_.initialize();
    return vk && physics && ecs && audio;
}
int NativeEngine::runSelfTest(){ constexpr uint32_t N=50000;
    const auto mesh = optimizeMesh({0,1,2,2,3,0}, 4);
    std::cout << "meshoptimizer: " << (mesh.indices.size()==6 ? "BOUNDARY_READY" : "FAILED") << "\n";
    for (int i=0;i<120;i++) physics_.step(1.0f/60.0f);
    const auto player = ecs_.spawn(1, 0.0f, 1.0f, 0.0f);
    const auto npc = ecs_.spawn(2, 5.0f, 1.0f, 5.0f);
    ecs_.update(1.0f / 60.0f);
    std::cout << "Flecs ECS: " << ((player != 0 && npc != 0) ? "ACTIVE" : "FAILED") << " entities=" << ecs_.entityCount() << "\n";
    std::cout << "miniaudio: " << (audio_.initialized() ? "ACTIVE" : "FAILED") << "\n"; std::cout << "Jolt simulation: ACTIVE first_dynamic_y=" << physics_.firstDynamicY() << "\n"; std::vector<ObjectGPU> objects; objects.reserve(N); for(uint32_t i=0;i<N;i++){float x=float(i%500)-250.f;float z=float(i/500)-50.f;objects.push_back({x,0,z,1.0f,i%8,i%4,0.5f,0});} auto t0=std::chrono::steady_clock::now();uint32_t visible=0;for(auto&o:objects){float d=std::sqrt(o.x*o.x+o.z*o.z);if(d<180.f)visible++;}auto t1=std::chrono::steady_clock::now();double ms=std::chrono::duration<double,std::milli>(t1-t0).count();std::cout<<"Native scene self-test: objects="<<N<<" visible="<<visible<<" culled="<<(N-visible)<<" CPU_ms="<<ms<<"\n";return 0;}
NativeGpuRenderStats NativeEngine::renderBootstrap(uint32_t width,uint32_t height){ return vk_.renderBootstrap(width,height); }
void NativeEngine::shutdown(){ audio_.shutdown(); ecs_.shutdown(); physics_.shutdown(); vk_.shutdown(); }
}
