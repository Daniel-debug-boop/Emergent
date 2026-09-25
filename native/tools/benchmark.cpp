#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

struct Object { float x,z,r; };

static uint64_t run(uint32_t n, float maxDistance) {
    std::vector<Object> objects;
    objects.reserve(n);
    for (uint32_t i=0;i<n;i++) {
        float x=float(i%1000)-500.f;
        float z=float(i/1000)-float(n/2000);
        objects.push_back({x,z,1.f});
    }
    uint64_t visible=0;
    auto t0=std::chrono::steady_clock::now();
    for (const auto& o: objects) {
        float d=std::sqrt(o.x*o.x+o.z*o.z);
        if (d-o.r <= maxDistance) ++visible;
    }
    auto t1=std::chrono::steady_clock::now();
    double ms=std::chrono::duration<double,std::milli>(t1-t0).count();
    std::cout << "objects=" << n << " visible=" << visible
              << " culled=" << (n-visible) << " cpu_ms=" << ms << '\n';
    return visible;
}

int main() {
    std::cout << "EMERGENT native benchmark lab (CPU reference only)\n";
    for (uint32_t n : {10000u, 50000u, 100000u, 250000u}) run(n, 180.f);
    return 0;
}
