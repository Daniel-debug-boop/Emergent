#include "emergent/engine.hpp"
#include <iostream>
int main(){emergent::NativeEngine e;if(!e.initialize()){std::cerr<<"Native engine initialization incomplete: "<<e.vulkan().error<<"\n";std::cerr<<"Native engine remains runnable in capability-probe/self-test mode.\n";e.runSelfTest();e.shutdown();return 0;}const auto&c=e.vulkan();std::cout<<"EMERGENT Vulkan backend initialized\nDevice: "<<c.device_name<<"\nAPI: "<<c.api_version<<"\nCompute: "<<(c.compute?"YES":"NO")<<"\nIndirect draw: "<<(c.indirect_draw?"YES":"NO")<<"\nDescriptor indexing: "<<(c.descriptor_indexing?"YES":"NO")<<"\nSubgroup: "<<(c.subgroup?"YES":"NO")<<"\nMesh shader: "<<(c.mesh_shader?"YES":"NO")<<"\nTask shader: "<<(c.task_shader?"YES":"NO")<<"\n";auto gpu=e.renderBootstrap(256,256);
std::cout<<"Native GPU bootstrap: "<<(gpu.executed?"EXECUTED":"NOT EXECUTED")<<" ";
if(gpu.executed) std::cout<<"submit_ms="<<gpu.submit_ms<<"\n"; else std::cout<<"error="<<gpu.error<<"\n";
int r=e.runSelfTest();e.shutdown();return r;}
