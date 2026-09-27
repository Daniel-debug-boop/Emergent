// EMERGENT native entry point.
//
// Three modes, chosen by flag:
//
//   (no flags)         capabilities, then the self-test. What CI runs.
//   --frames N         run N fixed-delta frames through the loop
//   --realtime S       run S seconds of real time, paced against the clock
//   --render WxH       attach the Vulkan renderer and open a target
//   --walk             drive the character forward for the duration, so the
//                      physics, the locomotion blend and the pose all move
//
// --render with no window attached renders offscreen: real Vulkan work with
// nothing presented, which is what a server build and a frame-capture harness
// want. Attaching a window is setSurfaceProvider() on the engine, and there is
// no windowing dependency in this tree -- see render_backend.hpp for why.

#include "emergent/engine.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void printUsage(const char *argv0) {
    std::printf(
        "usage: %s [--frames N] [--hz R] [--realtime S] [--render WxH] [--walk] [--width W] [--height H]\n"
        "\n"
        "  --frames N     run N frames through the fixed-timestep loop\n"
        "  --hz R         frame rate to run those frames at (default 60)\n"
        "                 the physics step stays 1/60s; R only changes how much\n"
        "                 real time each frame represents, which is exactly\n"
        "                 what a display refresh rate does\n"
        "  --realtime S   run S seconds paced against the monotonic clock\n"
        "  --render WxH   attach the Vulkan renderer and open a WxH target\n"
        "  --width W      framebuffer width (default 1280)\n"
        "  --height H     framebuffer height (default 720)\n"
        "  --walk         hold the forward axis for the duration\n"
        "  --assets DIR   where the packed KTX2 material maps are\n"
        "                 (default build/native-assets, relative to the cwd)\n"
        "  --scene [N]    generate a world for seed N and stream a slice of it to the\n"
        "                  renderer, then report what was built\n",
        argv0);
}

bool parseUint(const char *text, uint32_t &out) {
    if (text == nullptr || *text == '\0') return false;
    char *end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0') return false;
    out = static_cast<uint32_t>(value);
    return true;
}

bool parseDouble(const char *text, double &out) {
    if (text == nullptr || *text == '\0') return false;
    char *end = nullptr;
    out = std::strtod(text, &end);
    return end != text && *end == '\0';
}

} // namespace

int main(int argc, char **argv) {
    uint32_t frames = 0;
    uint32_t hz = 60;
    double realtime = 0.0;
    bool wantRender = false;
    bool walk = false;
    bool scene = false;
    int32_t seed = 7;
    uint32_t width = 1280;
    uint32_t height = 720;
    std::string assetDir;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const bool hasValue = (i + 1) < argc;
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        }
        if (std::strcmp(arg, "--frames") == 0 && hasValue) {
            if (!parseUint(argv[++i], frames)) {
                std::fprintf(stderr, "--frames needs a whole number\n");
                return 2;
            }
        } else if (std::strcmp(arg, "--hz") == 0 && hasValue) {
            if (!parseUint(argv[++i], hz) || hz == 0 || hz > 1000) {
                std::fprintf(stderr, "--hz needs a whole number between 1 and 1000\n");
                return 2;
            }
        } else if (std::strcmp(arg, "--realtime") == 0 && hasValue) {
            if (!parseDouble(argv[++i], realtime) || realtime <= 0.0) {
                std::fprintf(stderr, "--realtime needs a positive number of seconds\n");
                return 2;
            }
        } else if (std::strcmp(arg, "--width") == 0 && hasValue) {
            if (!parseUint(argv[++i], width) || width == 0) {
                std::fprintf(stderr, "--width needs a positive whole number\n");
                return 2;
            }
        } else if (std::strcmp(arg, "--height") == 0 && hasValue) {
            if (!parseUint(argv[++i], height) || height == 0) {
                std::fprintf(stderr, "--height needs a positive whole number\n");
                return 2;
            }
        } else if (std::strcmp(arg, "--render") == 0) {
            wantRender = true;
            if (hasValue && argv[i + 1][0] != '\0' && std::strchr(argv[i + 1], 'x') != nullptr) {
                char *end = nullptr;
                const unsigned long w = std::strtoul(argv[i + 1], &end, 10);
                if (*end == 'x') {
                    const unsigned long h = std::strtoul(end + 1, &end, 10);
                    if (*end == '\0' && w > 0 && h > 0) {
                        width = static_cast<uint32_t>(w);
                        height = static_cast<uint32_t>(h);
                        ++i;
                    }
                }
            }
        } else if (std::strcmp(arg, "--walk") == 0) {
            walk = true;
        } else if (std::strcmp(arg, "--scene") == 0) {
            scene = true;
            if (hasValue) {
                char *end = nullptr;
                const long v = std::strtol(argv[i + 1], &end, 10);
                if (end != argv[i + 1] && *end == '\0') {
                    seed = static_cast<int32_t>(v);
                    ++i;  // consumed, so it is not re-parsed as an argument
                }
            }
        } else if (std::strcmp(arg, "--assets") == 0) {
            // Where the packed KTX2 material maps live. Without this the
            // renderer falls back to a path relative to the working directory,
            // which silently finds nothing when the binary is launched from
            // anywhere but the repository root.
            if (!hasValue) {
                std::fprintf(stderr, "--assets needs a directory\n");
                return 2;
            }
            assetDir = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n\n", arg);
            printUsage(argv[0]);
            return 2;
        }
    }

    emergent::NativeEngine engine;
    if (!assetDir.empty()) {
        engine.setSceneAssetDirectory(assetDir);
    }
    const bool initialized = engine.initialize();
    if (!initialized) {
        std::fprintf(stderr, "Native engine initialization incomplete: %s\n",
                     engine.vulkan().error.c_str());
        std::fprintf(stderr,
                     "Native engine remains runnable in capability-probe/self-test mode.\n");
    } else {
        const auto &c = engine.vulkan();
        std::cout << "EMERGENT Vulkan backend initialized\n"
                     "Device: "
                  << c.device_name << "\nAPI: " << c.api_version << "\nCompute: " << (c.compute ? "YES" : "NO")
                  << "\nIndirect draw: " << (c.indirect_draw ? "YES" : "NO")
                  << "\nDescriptor indexing: " << (c.descriptor_indexing ? "YES" : "NO")
                  << "\nSubgroup: " << (c.subgroup ? "YES" : "NO")
                  << "\nMesh shader: " << (c.mesh_shader ? "YES" : "NO")
                  << "\nTask shader: " << (c.task_shader ? "YES" : "NO") << "\n";
        const auto gpu = engine.renderBootstrap(256, 256);
        std::cout << "Native GPU bootstrap: " << (gpu.executed ? "EXECUTED" : "NOT EXECUTED") << " ";
        if (gpu.executed) {
            std::cout << "submit_ms=" << gpu.submit_ms << "\n";
        } else {
            std::cout << "error=" << gpu.error << "\n";
        }
    }

    // -- the streamed world ------------------------------------------------
    if (scene) {
        // Reportable without a GPU on purpose. The renderer used to draw instanced
        // cubes because nothing connected the world generator to a vertex buffer;
        // this is the line that proves the connection is made, and it is checked
        // in CI where there is no Vulkan driver at all.
        engine.buildWorld(seed);
        engine.setStreamRadius(420.0);
        std::printf("World seed %d: %zu buildings, %zu roads, %zu trees\n", seed,
                    engine.world().buildings.size(), engine.world().roads.size(),
                    engine.world().trees.size());
        std::printf("Downtown: (%.0f, %.0f), %.1f buildings/hectare\n", engine.worldCentre().x,
                    engine.worldCentre().z, engine.worldCentre().density);
        std::printf("Material maps: %s\n", engine.sceneAssetDirectory().c_str());
    }

    // -- render target -----------------------------------------------------
    bool rendering = false;
    if (wantRender) {
        // Deliberately engine-owned. A windowing backend is a SurfaceProvider
        // passed to setSurfaceProvider() before initialize(); there is none in
        // this tree, so this opens an offscreen target and says so.
        if (engine.enableVulkanRenderer()) {
            rendering = engine.renderTo(width, height, nullptr);
        }
        if (rendering) {
            const auto &rs = engine.renderStats();
            std::printf("Render target %ux%u: %s (presents=%s, device=%s)\n", rs.width, rs.height,
                        rs.status.c_str(), engine.renderer().presentsToDisplay() ? "yes" : "no",
                        rs.hasDevice ? "yes" : "no");
            // Say so either way. A silently untextured city looks like a
            // material bug rather than a missing file, and the two have
            // completely different fixes.
            std::printf("Material maps: %s", rs.materialMapsLoaded ? "CC0 bake" : "NEUTRAL PLACEHOLDERS");
            if (!rs.materialMapNote.empty()) {
                std::printf(" (%s)", rs.materialMapNote.c_str());
            }
            std::printf("\n");
        } else {
            std::printf("Render target %ux%u NOT opened: %s\n", width, height,
                        engine.lastRenderError().c_str());
            std::printf("The frame loop still runs; the headless renderer is counting its frames.\n");
        }
    }

    // -- the loop ----------------------------------------------------------
    uint32_t ran = 0;
    if (walk) {
        emergent::InputState input;
        input.moveZ = 1.0f;
        engine.setInput(input);
    }
    if (frames > 0) {
        // The frame delta is 1/hz while the physics step stays 1/60. That is
        // the whole point: at 144Hz each frame represents less time than a
        // physics step, so most frames take no step at all, and at 30Hz each
        // one takes two. The simulation has to come out the same either way.
        ran = engine.runHeadless(frames, 1.0 / static_cast<double>(hz));
        std::printf("Requested %uHz: each frame is %.4fms, the physics step is %.4fms\n", hz,
                    1000.0 / static_cast<double>(hz), 1000.0 * engine.loop().config().fixedDelta);
    } else if (realtime > 0.0) {
        ran = engine.runRealtime(realtime);
    }

    if (scene) {
        // What the renderer would have been handed. Printed rather than asserted
        // here so a human running the binary sees the same numbers CI checks.
        const auto &st = engine.sceneStats();
        std::printf("Scene slice: %u buildings, %u roads, %u props, %u trees, %u ground quads\n",
                    st.buildings, st.roads, st.props, st.trees, st.groundQuads);
        std::printf("Scene mesh: %u vertices, %u triangles, %u dropped for budget\n", st.vertices,
                    st.triangles, st.buildingsDroppedForBudget);
    }
    if (frames > 0 || realtime > 0.0) {
        const auto &st = engine.frameStats();
        const auto &last = engine.loop().lastFrame();
        // alpha is printed with %.17g because it is a double on a boundary
        // case: at 144Hz the accumulator repeatedly lands one ulp under a
        // step, so alpha is 0.9999999999999999. Any fixed-precision format
        // rounds that to "1", which reads like the documented [0,1) contract
        // has been broken when it has in fact been held. The remainder in
        // milliseconds is printed alongside because that is the number that
        // actually explains the frame.
        std::printf("Frame loop: ran=%u frame=%llu steps=%d sim=%.3fs alpha=%.17g (%.3fms to the next "
                    "step) clip=%.*s speed=%.2f grounded=%d pos=(%.2f,%.2f,%.2f) joints=%d boxes=%d\n",
                    ran, static_cast<unsigned long long>(st.frameIndex), st.steps, st.simulatedSeconds, st.alpha,
                    st.alpha * engine.loop().config().fixedDelta * 1000.0,
                    static_cast<int>(engine.loop().currentClip().size()), engine.loop().currentClip().data(),
                    static_cast<double>(engine.loop().playerSpeed()), engine.loop().grounded() ? 1 : 0,
                    static_cast<double>(last.playerPosition[0]), static_cast<double>(last.playerPosition[1]),
                    static_cast<double>(last.playerPosition[2]), last.jointCount, last.sceneBoxCount);
        std::printf("Frame timing: physics=%.3fms animation=%.3fms total=%.3fms\n", st.physicsMs, st.animationMs,
                    st.totalMs);
        if (rendering) {
            const auto &rs = engine.renderStats();
            std::printf("Render: submitted=%llu skipped=%llu status=%s\n",
                        static_cast<unsigned long long>(rs.submittedFrames),
                        static_cast<unsigned long long>(rs.skippedFrames), rs.status.c_str());
        }
    }

    const int result = engine.runSelfTest();
    engine.shutdown();
    return result;
}
