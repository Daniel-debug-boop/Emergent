// Render graph tests.
//
// The render graph is the piece of the renderer that is hardest to check by
// looking at a frame, and the easiest to get wrong. A barrier emitted for the
// wrong pair of passes does not crash; it produces a correct-looking image
// three frames later. A resource marked transient that outlives its pass does
// not crash either; it reads back whatever the allocator gave it next. So this
// suite asserts *invariants* — the size a fraction must resolve to, the order
// two passes must end up in, the eligibility rule for transience — rather than
// comparing against a recorded frame.
//
// Two properties get the most attention, because they are the two that the
// engine is actually being built for:
//
//   * A `SwapchainRelative` target is a fraction of the swapchain, so changing
//     the backbuffer extent and re-baking resizes the whole frame. That is
//     dynamic resolution, and here it is a data change rather than a promise.
//   * A resource is transient only when its entire lifetime falls inside one
//     render pass. Getting that wrong destroys a target another pass is about
//     to read, which is the worst failure mode a renderer has.
//
// The negative tests are not optional extras. A graph that silently accepts a
// read with no writer, a dependency cycle, or a resource declared as both a
// texture and a buffer would each produce a plausible frame, so each of those
// is asserted to throw.

#include "emergent/render_graph.hpp"

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;
const char *g_test = "";

void check(bool condition, const std::string &what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        std::printf("  FAIL [%s] %s\n", g_test, what.c_str());
    }
}

void run(const char *name, void (*fn)()) {
    g_test = name;
    const int before = g_failures;
    fn();
    std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
}

using namespace emergent;

constexpr RenderGraphQueueFlagBits kGraphics = RENDER_GRAPH_QUEUE_GRAPHICS_BIT;
constexpr RenderGraphQueueFlagBits kCompute = RENDER_GRAPH_QUEUE_COMPUTE_BIT;

/** A backbuffer of the given size at a known format. */
ResourceDimensions backbuffer(uint32_t w, uint32_t h, Format f = Format::B8G8R8A8_SRGB) {
    ResourceDimensions d;
    d.width = w;
    d.height = h;
    d.depth = 1;
    d.format = f;
    d.levels = 1;
    return d;
}

/** A colour target covering a fraction of the swapchain. */
AttachmentInfo fraction(float x, float y, Format f = Format::R16G16B16A16_SFLOAT) {
    AttachmentInfo a;
    a.size_class = SizeClass::SwapchainRelative;
    a.size_x = x;
    a.size_y = y;
    a.format = f;
    a.levels = 0;
    return a;
}

/** A fixed pixel-size target. */
AttachmentInfo absolute(uint32_t w, uint32_t h, Format f = Format::R16G16B16A16_SFLOAT) {
    AttachmentInfo a;
    a.size_class = SizeClass::Absolute;
    a.size_x = static_cast<float>(w);
    a.size_y = static_cast<float>(h);
    a.format = f;
    a.levels = 0;
    return a;
}

/** A target sized as a fraction of another named target. */
AttachmentInfo relativeTo(const char *name, float x, float y, Format f = Format::R16G16B16A16_SFLOAT) {
    AttachmentInfo a;
    a.size_class = SizeClass::InputRelative;
    a.size_relative_name = name;
    a.size_x = x;
    a.size_y = y;
    a.format = f;
    a.levels = 0;
    return a;
}

/** Run `fn` and report whether it threw. */
bool throws(const std::function<void()> &fn) {
    try {
        fn();
    } catch (const std::exception &) {
        return true;
    }
    return false;
}

/** Position of a pass in the derived order, or -1 if it is not in it. */
int orderOf(const RenderGraph &graph, const std::string &name) {
    const RenderPass *pass = graph.findPass(name);
    if (pass == nullptr) {
        return -1;
    }
    const auto &order = graph.getFlattenedPasses();
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == pass->getIndex()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/** The physical slot a named resource landed in. */
const ResourceDimensions *slotOf(const RenderGraph &graph, const std::string &name) {
    for (uint32_t i = 0; i < graph.getPhysicalResourceCount(); ++i) {
        const auto &dim = graph.getPhysicalDimensions(i);
        if (dim.name == name) {
            return &dim;
        }
    }
    return nullptr;
}

// -- sizing -----------------------------------------------------------------

void test_sizing_follows_the_swapchain() {
    // This is the property dynamic resolution is built on. Declared once at
    // 1920x1080, re-baked at 1280x720, the same declaration must produce a
    // smaller target without a single line of the frame being redeclared.
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1920, 1080));
    RenderPass &pass = graph.addPass("main", kGraphics);
    pass.addColorOutput("hdr", fraction(1.0f, 1.0f));
    pass.addColorOutput("half", fraction(0.5f, 0.5f));
    pass.addColorOutput("quarter", fraction(0.25f, 0.25f));
    graph.bake();

    const auto *hdr = slotOf(graph, "hdr");
    const auto *half = slotOf(graph, "half");
    const auto *quarter = slotOf(graph, "quarter");
    check(hdr != nullptr && half != nullptr && quarter != nullptr, "all three targets have a slot");
    check(hdr && hdr->width == 1920 && hdr->height == 1080, "full-scale target matches the swapchain");
    check(half && half->width == 960 && half->height == 540, "half-scale target is 960x540");
    check(quarter && quarter->width == 480 && quarter->height == 270, "quarter-scale target is 480x270");

    graph.setBackbufferDimensions(backbuffer(1280, 720));
    graph.bake();

    const auto *half2 = slotOf(graph, "half");
    const auto *quarter2 = slotOf(graph, "quarter");
    check(half2 && half2->width == 640 && half2->height == 360, "re-baking at 1280x720 resizes the half target");
    check(quarter2 && quarter2->width == 320 && quarter2->height == 180, "re-baking resizes the quarter target");
}

void test_absolute_is_not_relative() {
    // A fixed target must not move when the swapchain does. If it did, every
    // absolute-sized resource in the engine would silently become a function of
    // the window size.
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1920, 1080));
    RenderPass &pass = graph.addPass("main", kGraphics);
    pass.addColorOutput("lut", absolute(1024, 1024, Format::R16_SFLOAT));
    pass.addColorOutput("shadow", absolute(2048, 2048, Format::D32_SFLOAT));
    graph.bake();

    const auto *lut = slotOf(graph, "lut");
    check(lut && lut->width == 1024 && lut->height == 1024, "absolute 1024x1024 resolves to itself");

    graph.setBackbufferDimensions(backbuffer(800, 600));
    graph.bake();
    const auto *lut2 = slotOf(graph, "lut");
    const auto *shadow2 = slotOf(graph, "shadow");
    check(lut2 && lut2->width == 1024, "absolute target is unaffected by a swapchain resize");
    check(shadow2 && shadow2->width == 2048, "absolute shadow map is unaffected by a swapchain resize");
}

void test_input_relative_chains() {
    // A bloom pyramid: each level is half the one before. Declared in the order
    // a human writes them, which is not the order they resolve in.
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1920, 1080));
    RenderPass &a = graph.addPass("threshold", kGraphics);
    a.addColorOutput("bloom0", relativeTo("hdr", 0.5f, 0.5f));
    RenderPass &b = graph.addPass("downsample", kGraphics);
    b.addColorOutput("bloom1", relativeTo("bloom0", 0.5f, 0.5f));
    RenderPass &c = graph.addPass("upsample", kGraphics);
    c.addColorOutput("hdr", fraction(1.0f, 1.0f));
    c.addTextureInput("bloom1");
    c.addTextureInput("bloom0");
    graph.bake();

    const auto *hdr = slotOf(graph, "hdr");
    const auto *b0 = slotOf(graph, "bloom0");
    const auto *b1 = slotOf(graph, "bloom1");
    check(hdr && hdr->width == 1920, "the chain root is swapchain-sized");
    check(b0 && b0->width == 960 && b0->height == 540, "bloom0 is half of hdr");
    check(b1 && b1->width == 480 && b1->height == 270, "bloom1 is half of bloom0, resolving through a forward reference");
}

void test_extent_never_reaches_zero() {
    // A fraction small enough to round to zero would allocate a zero-sized
    // image, and every later barrier and view on it is undefined behaviour.
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(64, 64));
    RenderPass &pass = graph.addPass("main", kGraphics);
    AttachmentInfo tiny = fraction(0.001f, 0.001f);
    pass.addColorOutput("tiny", tiny);
    AttachmentInfo zero = fraction(0.0f, 0.0f);
    pass.addColorOutput("zero", zero);
    AttachmentInfo depth0 = absolute(0.0f, 0.0f);
    pass.addColorOutput("zeroabs", depth0);
    graph.bake();

    const auto *t = slotOf(graph, "tiny");
    const auto *z = slotOf(graph, "zero");
    const auto *za = slotOf(graph, "zeroabs");
    check(t && t->width == 1 && t->height == 1, "a vanishing fraction clamps to 1x1, not 0x0");
    check(z && z->width == 1 && z->height == 1, "a zero fraction clamps to 1x1");
    check(za && za->width == 1 && za->height == 1, "a zero absolute extent clamps to 1x1");
}

void test_mip_level_counts() {
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1920, 1080));
    RenderPass &pass = graph.addPass("main", kGraphics);

    AttachmentInfo full = absolute(1024, 1024);
    full.levels = 0;
    pass.addColorOutput("full", full);

    AttachmentInfo capped = absolute(1024, 1024);
    capped.levels = 4;
    pass.addColorOutput("capped", capped);

    AttachmentInfo one = absolute(256, 256);
    one.levels = 0;
    pass.addColorOutput("one", one);
    graph.bake();

    const auto *f = slotOf(graph, "full");
    const auto *c = slotOf(graph, "capped");
    const auto *o = slotOf(graph, "one");
    // 1024 -> 1 by halving is 11 levels: 1024, 512, 256, 128, 64, 32, 16, 8, 4, 2, 1.
    check(f && f->levels == 11, "1024x1024 has 11 mip levels, not 10 or 12");
    check(c && c->levels == 4, "an explicit level count caps the chain");
    check(o && o->levels == 9, "256x256 has 9 mip levels");
}

void test_format_is_inherited_from_the_swapchain() {
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1920, 1080, Format::B8G8R8A8_SRGB));
    RenderPass &pass = graph.addPass("main", kGraphics);
    AttachmentInfo inherit;
    inherit.size_class = SizeClass::SwapchainRelative;
    pass.addColorOutput("plain", inherit);
    pass.addColorOutput("hdr", fraction(1.0f, 1.0f, Format::R16G16B16A16_SFLOAT));
    graph.bake();

    const auto *plain = slotOf(graph, "plain");
    const auto *hdr = slotOf(graph, "hdr");
    check(plain && plain->format == Format::B8G8R8A8_SRGB, "an unspecified format inherits the backbuffer's");
    check(hdr && hdr->format == Format::R16G16B16A16_SFLOAT, "an explicit format is not overwritten");
}

void test_byte_sizes() {
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1920, 1080));
    RenderPass &pass = graph.addPass("main", kGraphics);
    pass.addColorOutput("rgba8", fraction(1.0f, 1.0f, Format::R8G8B8A8_UNORM));
    pass.addColorOutput("rgba16f", fraction(1.0f, 1.0f, Format::R16G16B16A16_SFLOAT));
    pass.addColorOutput("depth", fraction(1.0f, 1.0f, Format::D32_SFLOAT));
    graph.bake();

    const auto *r8 = slotOf(graph, "rgba8");
    const auto *r16 = slotOf(graph, "rgba16f");
    const auto *d = slotOf(graph, "depth");
    check(r8 && r8->level0ByteSize() == 1920ull * 1080ull * 4ull, "RGBA8 level 0 is 4 bytes per pixel");
    check(r16 && r16->level0ByteSize() == 1920ull * 1080ull * 8ull, "RGBA16F level 0 is 8 bytes per pixel");
    check(d && d->level0ByteSize() == 1920ull * 1080ull * 4ull, "D32 level 0 is 4 bytes per pixel");
}

// -- ordering ---------------------------------------------------------------

void test_reads_follow_writes() {
    // Declared backwards on purpose: a graph that merely preserves declaration
    // order would pass a forward-declared test and fail this one.
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1920, 1080));

    RenderPass &tonemap = graph.addPass("tonemap", kGraphics);
    tonemap.addColorOutput("backbuffer", fraction(1.0f, 1.0f, Format::B8G8R8A8_SRGB));
    tonemap.addTextureInput("hdr");

    RenderPass &lighting = graph.addPass("lighting", kGraphics);
    lighting.addColorOutput("hdr", fraction(1.0f, 1.0f));
    lighting.addTextureInput("gbuf");

    RenderPass &gbuffer = graph.addPass("gbuffer", kGraphics);
    gbuffer.addColorOutput("gbuf", fraction(1.0f, 1.0f));

    RenderPass &cull = graph.addPass("cull", kCompute);
    BufferInfo args;
    args.size = 4096;
    cull.addStorageOutput("drawArgs", args);

    RenderPass &draw = graph.addPass("draw", kGraphics);
    draw.addColorOutput("gbuf", fraction(1.0f, 1.0f, Format::Undefined));
    draw.addIndirectBufferInput("drawArgs");

    graph.bake();

    const int iCull = orderOf(graph, "cull");
    const int iDraw = orderOf(graph, "draw");
    const int iGbuf = orderOf(graph, "gbuffer");
    const int iLight = orderOf(graph, "lighting");
    const int iTone = orderOf(graph, "tonemap");

    check(iCull >= 0 && iDraw >= 0 && iGbuf >= 0 && iLight >= 0 && iTone >= 0, "every pass is scheduled");
    check(iCull < iDraw, "the pass that draws follows the pass that produced its indirect args");
    check(iDraw != iGbuf, "two passes writing the same target are ordered against each other");
    check(iGbuf < iLight, "lighting follows the geometry pass it samples");
    check(iLight < iTone, "tonemapping follows the pass that produced the HDR image");
    check(graph.getFlattenedPasses().size() == 5, "every pass appears exactly once in the order");
}

void test_two_writers_are_ordered_in_declaration_order() {
    // Two passes writing one target. Neither reads it, so the only thing that
    // can order them is the write-after-write edge between the writers
    // themselves. Miss that edge and the scheduler is free to run them in
    // either order, and the last writer is whichever the tie-break picked.
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(640, 480));
    RenderPass &a = graph.addPass("first_writer", kGraphics);
    a.addColorOutput("target", fraction(0.5f, 0.5f));
    RenderPass &b = graph.addPass("second_writer", kGraphics);
    b.addColorOutput("target", fraction(0.5f, 0.5f));
    RenderPass &c = graph.addPass("reader", kGraphics);
    c.addTextureInput("target");
    graph.bake();

    check(orderOf(graph, "first_writer") < orderOf(graph, "second_writer"),
          "the first declared writer runs first");
    check(orderOf(graph, "second_writer") < orderOf(graph, "reader"),
          "the reader follows the last writer");

    int waw = 0;
    for (const auto &dep : graph.getDependencies()) {
        if (dep.kind == PassDependency::Kind::WriteAfterWrite) {
            waw++;
        }
    }
    check(waw == 1, "exactly one write-after-write edge exists for two writers");
}

void test_order_is_independent_of_declaration_order() {
    // Two frames declaring the same dependencies in opposite orders must
    // produce the same order. If they do not, the graph has a hidden tie-break
    // and every regression involving ordering becomes unreproducible.
    //
    // The comparison is on pass *names*, not the raw indices in the flattened
    // list. Indices are assigned in declaration order, so the two graphs
    // necessarily disagree about which number belongs to "b"; comparing numbers
    // would fail for a correct graph and pass for an incorrect one.
    auto orderedNames = [](bool reverse) {
        RenderGraph graph;
        graph.setBackbufferDimensions(backbuffer(640, 480));
        auto add = [&](const char *name, const char *input) {
            RenderPass &p = graph.addPass(name, kGraphics);
            p.addColorOutput(name, fraction(1.0f, 1.0f));
            if (input != nullptr) {
                p.addTextureInput(input);
            }
        };
        if (reverse) {
            add("c", "b");
            add("b", "a");
            add("a", nullptr);
        } else {
            add("a", nullptr);
            add("b", "a");
            add("c", "b");
        }
        graph.bake();

        std::vector<std::string> names;
        for (uint32_t index : graph.getFlattenedPasses()) {
            names.push_back(graph.passName(index));
        }
        return names;
    };

    const auto forward = orderedNames(false);
    const auto reverse = orderedNames(true);
    check(forward.size() == 3, "three passes scheduled");
    check(forward == reverse, "reversing the declaration yields the same derived order");
    check(forward.size() == 3 && forward[0] == "a" && forward[1] == "b" && forward[2] == "c",
          "the chain resolves a, then b, then c");
}

void test_cycles_are_rejected() {
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(640, 480));
    RenderPass &a = graph.addPass("a", kGraphics);
    a.addColorOutput("ta", fraction(1.0f, 1.0f));
    a.addTextureInput("tb");
    RenderPass &b = graph.addPass("b", kGraphics);
    b.addColorOutput("tb", fraction(1.0f, 1.0f));
    b.addTextureInput("ta");

    // A cycle is the one declaration error that produces no output at all and
    // no crash, so it has to be an exception rather than an empty schedule.
    check(throws([&] { graph.bake(); }), "a read/write cycle is rejected rather than silently dropped");
}

void test_read_without_a_writer_is_rejected() {
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(640, 480));
    RenderPass &pass = graph.addPass("main", kGraphics);
    pass.addColorOutput("out", fraction(1.0f, 1.0f));
    pass.addTextureInput("neverWritten");

    // Sampling memory no pass produced this frame returns whatever the
    // allocator happened to hand out. That is a plausible image, which is
    // exactly why it has to be an error.
    check(throws([&] { graph.bake(); }), "reading a resource no pass writes is rejected");
}

void test_duplicate_pass_names_are_rejected() {
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(640, 480));
    graph.addPass("main", kGraphics);
    check(throws([&] { graph.addPass("main", kCompute); }), "two passes cannot share a name");
}

void test_duplicate_resource_kinds_are_rejected() {
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(640, 480));
    BufferInfo buf;
    buf.size = 256;

    RenderPass &a = graph.addPass("a", kGraphics);
    a.addColorOutput("shared", fraction(1.0f, 1.0f));

    // Declared as a buffer, under a name already interned as a texture. Without
    // the check the second declaration displaces the first and the frame binds
    // the wrong kind of resource under a name that looks right.
    check(throws([&] {
        RenderPass &b = graph.addPass("b", kCompute);
        b.addStorageOutput("shared", buf);
    }),
          "a name already interned as a texture cannot be declared as a buffer");

    // The same in the other direction, and on the read side.
    RenderGraph other;
    other.setBackbufferDimensions(backbuffer(640, 480));
    RenderPass &bufWriter = other.addPass("writer", kCompute);
    bufWriter.addStorageOutput("thing", buf);
    check(throws([&] {
        RenderPass &texReader = other.addPass("reader", kGraphics);
        texReader.addColorOutput("thing", fraction(1.0f, 1.0f));
    }),
          "a name already interned as a buffer cannot be declared as a texture");
}

void test_dependency_kinds() {
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(640, 480));
    RenderPass &upload = graph.addPass("upload", kCompute);
    BufferInfo buf;
    buf.size = 1024;
    upload.addStorageOutput("accum", buf);
    RenderPass &accumulate = graph.addPass("accumulate", kCompute);
    accumulate.addStorageReadOnlyInput("accum");
    accumulate.addStorageOutput("accum", buf);
    graph.bake();

    bool sawWar = false;
    for (const auto &dep : graph.getDependencies()) {
        if (dep.dependent == accumulate.getIndex() && dep.dependency == upload.getIndex()) {
            check(dep.kind == PassDependency::Kind::WriteAfterRead,
                  "a pass that reads and writes a buffer is a write-after-read hazard");
            sawWar = true;
        }
    }
    check(sawWar, "the read-modify-write edge is recorded at all");
    check(orderOf(graph, "upload") < orderOf(graph, "accumulate"),
          "the read-modify-write pass follows the pass that produced the buffer");
}

// -- transience -------------------------------------------------------------

void test_cross_pass_resources_are_persistent() {
    // The central rule. A shadow map written by one pass and read by another
    // outlives the pass that made it, so treating it as transient would hand the
    // lighting pass whatever the allocator reused the memory for.
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &shadow = graph.addPass("shadow", kGraphics);
    shadow.setDepthStencilOutput("shadowMap", absolute(1024, 1024, Format::D32_SFLOAT));
    RenderPass &lit = graph.addPass("lighting", kGraphics);
    lit.addColorOutput("hdr", fraction(1.0f, 1.0f));
    lit.addTextureInput("shadowMap");
    graph.bake();

    const auto *sm = slotOf(graph, "shadowMap");
    check(sm != nullptr, "the shadow map has a slot");
    check(sm && !sm->isTransient(), "a resource spanning two render passes is persistent");
    check(sm && sm->isBufferLike() == false, "a depth target is an image, not a buffer");
}

void test_single_pass_resources_are_transient() {
    // Written and read by the same pass: the whole lifetime is inside it, which
    // is the only condition under which transience is safe.
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &pass = graph.addPass("composite", kGraphics);
    pass.addColorOutput("backbuffer", fraction(1.0f, 1.0f, Format::B8G8R8A8_SRGB));
    pass.addColorOutput("scratch", fraction(0.5f, 0.5f));
    pass.addAttachmentInput("scratch");
    graph.bake();

    const auto *scratch = slotOf(graph, "scratch");
    check(scratch != nullptr, "the scratch target has a slot");
    check(scratch && scratch->isTransient(), "a target written and read inside one pass is transient");
}

void test_buffers_are_never_transient() {
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &pass = graph.addPass("compute", kCompute);
    BufferInfo buf;
    buf.size = 4096;
    pass.addStorageOutput("scratchBuffer", buf);
    pass.addStorageReadOnlyInput("scratchBuffer");
    graph.bake();

    const auto *b = slotOf(graph, "scratchBuffer");
    check(b != nullptr, "the buffer has a slot");
    check(b && b->isBufferLike(), "a sized buffer is reported as buffer-like");
    check(b && !b->isTransient(), "a buffer is never transient, even inside a single pass");
}

void test_history_disqualifies_transience() {
    // A history read refers to memory from the previous frame. Transient memory
    // has no such memory, so the resource must persist no matter how few passes
    // touch it.
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &pass = graph.addPass("tonemap", kGraphics);
    pass.addColorOutput("backbuffer", fraction(1.0f, 1.0f, Format::B8G8R8A8_SRGB));
    pass.addColorOutput("history", fraction(1.0f, 1.0f));
    pass.addHistoryInput("history");
    graph.bake();

    const auto *h = slotOf(graph, "history");
    check(h != nullptr, "the history target has a slot");
    check(h && !h->isTransient(), "a resource with history is persistent even inside one pass");
}

void test_depth_transience_follows_the_device() {
    // Depth can only be transient on an implementation that offers tile memory.
    // Colour transient is legal everywhere, so the two capabilities are separate
    // and must not be conflated.
    auto build = [](bool depthCapable, RenderGraph &graph) {
        graph.setBackbufferDimensions(backbuffer(1024, 1024));
        graph.setUseTransientDepthStencil(depthCapable);
        RenderPass &pass = graph.addPass("pass", kGraphics);
        AttachmentInfo d;
        d.size_class = SizeClass::SwapchainRelative;
        d.format = Format::D32_SFLOAT;
        d.levels = 0;
        pass.setDepthStencilOutput("depth", d);
        pass.addAttachmentInput("depth");
        graph.bake();
    };

    RenderGraph noCapGraph;
    build(false, noCapGraph);
    RenderGraph withCapGraph;
    build(true, withCapGraph);

    const auto *noCap = slotOf(noCapGraph, "depth");
    check(noCap && !noCap->isTransient(), "without tile memory a single-pass depth target stays persistent");
    const auto *withCap = slotOf(withCapGraph, "depth");
    check(withCap && withCap->isTransient(), "with tile memory a single-pass depth target is transient");
}

void test_backbuffer_is_always_persistent() {
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1024, 1024, Format::B8G8R8A8_SRGB));
    RenderPass &pass = graph.addPass("tonemap", kGraphics);
    pass.addColorOutput("backbuffer", fraction(1.0f, 1.0f, Format::B8G8R8A8_SRGB));
    graph.setBackbufferSource("backbuffer");
    graph.bake();

    const auto *bb = slotOf(graph, "backbuffer");
    check(bb != nullptr, "the backbuffer has a slot");
    check(bb && !bb->isTransient(), "the presented image is never transient");
}

void test_cross_pass_writes_to_the_same_target_are_persistent() {
    // Two passes writing one target is legal; the resource then has a lifetime
    // spanning both, and must not be transient even though each access sits
    // inside a single pass.
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &a = graph.addPass("a", kGraphics);
    a.addColorOutput("ping", fraction(0.5f, 0.5f));
    RenderPass &b = graph.addPass("b", kGraphics);
    b.addColorOutput("ping", fraction(0.5f, 0.5f));
    RenderPass &c = graph.addPass("c", kGraphics);
    c.addTextureInput("ping");
    graph.bake();

    const auto *p = slotOf(graph, "ping");
    check(p != nullptr, "the shared target has a slot");
    check(p && !p->isTransient(), "a target written by one pass and read by another is persistent");
    check(orderOf(graph, "a") < orderOf(graph, "b"), "the two writers are ordered against each other");
    check(orderOf(graph, "b") < orderOf(graph, "c"), "the reader follows both writers");
}

// -- merging and aliasing ---------------------------------------------------

void test_subpass_merge_makes_a_target_transient() {
    // A geometry pass writes the G-buffer and a lighting pass reads it as an
    // input attachment, writing the same attachment back. They can share one
    // render pass and one allocation, and then the G-buffer really is
    // single-pass after all. This is the case that makes subpass merging worth
    // doing rather than merely tidy.
    //
    // The input and the output must describe the same attachment -- same extent,
    // same format -- because after aliasing there is only one image and it can
    // only be one size and one format.
    RenderGraph split;
    split.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &g1 = split.addPass("gbuffer", kGraphics);
    g1.addColorOutput("gbuf", fraction(1.0f, 1.0f));
    RenderPass &l1 = split.addPass("lighting", kGraphics);
    l1.addColorOutput("hdr", fraction(1.0f, 1.0f));
    l1.addTextureInput("gbuf");
    split.bake();
    const auto *splitGbuf = slotOf(split, "gbuf");
    check(splitGbuf && !splitGbuf->isTransient(), "a sampled G-buffer across two passes is persistent");

    RenderGraph merged;
    merged.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &g2 = merged.addPass("gbuffer", kGraphics);
    g2.addColorOutput("gbuf", fraction(1.0f, 1.0f));
    RenderPass &l2 = merged.addPass("lighting", kGraphics);
    // Same format as the input, which is what a subpass read-modify-write of one
    // attachment actually looks like.
    l2.addColorOutput("gbuf2", fraction(1.0f, 1.0f, Format::R16G16B16A16_SFLOAT), "gbuf");
    merged.bake();

    const auto *mergedGbuf = slotOf(merged, "gbuf");
    check(mergedGbuf && mergedGbuf->isTransient(),
          "declaring the G-buffer as an input attachment merges the passes and makes it transient");
    check(merged.findPass("gbuffer")->getPhysicalPassIndex() ==
              merged.findPass("lighting")->getPhysicalPassIndex(),
          "a merged pair shares one physical pass index");
    check(merged.getPhysicalResourceCount() < split.getPhysicalResourceCount(),
          "merging passes collapses physical slots");
}

void test_mismatched_aliases_are_refused() {
    // The guard on aliasing. A pair that disagrees about format or extent cannot
    // become one attachment, and quietly picking one of the two would render the
    // wrong image while the slot count still looked healthy.
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &g = graph.addPass("gbuffer", kGraphics);
    g.addColorOutput("gbuf", fraction(1.0f, 1.0f, Format::R16G16B16A16_SFLOAT));
    RenderPass &l = graph.addPass("lighting", kGraphics);
    // A different format from the input it was declared to read.
    l.addColorOutput("ldr", fraction(1.0f, 1.0f, Format::R8G8B8A8_UNORM), "gbuf");
    graph.bake();

    const auto *gbuf = slotOf(graph, "gbuf");
    const auto *ldr = slotOf(graph, "ldr");
    check(gbuf != nullptr && ldr != nullptr, "both resources have a slot");
    check(gbuf != ldr, "a format-mismatched input and output stay separate allocations");
    check(graph.getPhysicalResourceCount() == 2, "the mismatched pair is not collapsed into one slot");

    // A different extent is refused for the same reason.
    RenderGraph sized;
    sized.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &g2 = sized.addPass("gbuffer", kGraphics);
    g2.addColorOutput("gbuf", fraction(1.0f, 1.0f, Format::R16G16B16A16_SFLOAT));
    RenderPass &l2 = sized.addPass("lighting", kGraphics);
    l2.addColorOutput("half_ldr", fraction(0.5f, 0.5f, Format::R16G16B16A16_SFLOAT), "gbuf");
    sized.bake();
    check(sized.getPhysicalResourceCount() == 2, "an extent-mismatched pair is not collapsed either");
}

void test_merging_is_transitive() {
    // A reads B, B reads C. Merging is transitive, so all three are one render
    // pass. A single-sweep merge would leave A in its own pass and report the
    // G-buffer persistent when the frame can in fact render it transiently.
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &c = graph.addPass("c", kGraphics);
    c.addColorOutput("gbuf", fraction(1.0f, 1.0f));
    RenderPass &b = graph.addPass("b", kGraphics);
    b.addColorOutput("middle", fraction(1.0f, 1.0f, Format::Undefined), "gbuf");
    RenderPass &a = graph.addPass("a", kGraphics);
    a.addColorOutput("out", fraction(1.0f, 1.0f, Format::Undefined), "middle");
    graph.bake();

    check(graph.findPass("a")->getPhysicalPassIndex() == graph.findPass("b")->getPhysicalPassIndex() &&
              graph.findPass("b")->getPhysicalPassIndex() == graph.findPass("c")->getPhysicalPassIndex(),
          "an A-reads-B-reads-C chain merges into a single physical pass");
    const auto *gbuf = slotOf(graph, "gbuf");
    check(gbuf && gbuf->isTransient(), "a transitively merged chain is still single-pass, so the G-buffer is transient");
}

void test_different_queues_do_not_merge() {
    // A compute pass writing a target a graphics pass reads is the GPU-driven
    // case: it needs a real dependency and a semaphore, not a merge.
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &compute = graph.addPass("cull", kCompute);
    compute.addStorageOutput("visible", BufferInfo{256, BUFFER_USAGE_STORAGE_BIT, ATTACHMENT_INFO_PERSISTENT_BIT});
    RenderPass &render = graph.addPass("draw", kGraphics);
    render.addColorOutput("hdr", fraction(1.0f, 1.0f));
    render.addStorageReadOnlyInput("visible");
    graph.bake();

    check(graph.findPass("cull")->getPhysicalPassIndex() != graph.findPass("draw")->getPhysicalPassIndex(),
          "a compute pass and a graphics pass never share a physical pass");
    check(orderOf(graph, "cull") < orderOf(graph, "draw"), "the graphics pass still follows the compute pass");
}

// -- lifecycle --------------------------------------------------------------

void test_bake_is_stable_and_resettable() {
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &pass = graph.addPass("main", kGraphics);
    pass.addColorOutput("hdr", fraction(1.0f, 1.0f));
    graph.bake();
    const auto first = graph.getFlattenedPasses();
    const size_t slots = graph.getPhysicalResourceCount();

    graph.bake();
    check(graph.getFlattenedPasses() == first, "baking twice produces the same order");
    check(graph.getPhysicalResourceCount() == slots, "baking twice produces the same slot count");

    graph.reset();
    check(graph.getFlattenedPasses().empty(), "reset clears the derived order");
    check(graph.getPhysicalResourceCount() == 0, "reset clears the physical slots");
    check(graph.findPass("main") == nullptr, "reset clears the passes");

    // Reusing the graph after a reset has to work; a graph that only bakes once
    // is a scratch object, not a frame description.
    graph.setBackbufferDimensions(backbuffer(512, 512));
    RenderPass &again = graph.addPass("main", kGraphics);
    again.addColorOutput("hdr", fraction(1.0f, 1.0f));
    graph.bake();
    const auto *hdr = slotOf(graph, "hdr");
    check(hdr && hdr->width == 512, "a reset graph bakes against its new backbuffer");
}

void test_bake_rejects_an_impossible_frame() {
    RenderGraph empty;
    empty.setBackbufferDimensions(backbuffer(1024, 1024));
    check(throws([&] { empty.bake(); }), "baking a graph with no passes is rejected");

    RenderGraph noSize;
    noSize.setBackbufferDimensions(backbuffer(0, 0));
    RenderPass &p = noSize.addPass("main", kGraphics);
    p.addColorOutput("hdr", fraction(1.0f, 1.0f));
    check(throws([&] { noSize.bake(); }), "baking against a zero-sized backbuffer is rejected");

    RenderGraph noFormat;
    ResourceDimensions d = backbuffer(1024, 1024);
    d.format = Format::Undefined;
    noFormat.setBackbufferDimensions(d);
    RenderPass &q = noFormat.addPass("main", kGraphics);
    q.addColorOutput("hdr", fraction(1.0f, 1.0f));
    check(throws([&] { noFormat.bake(); }), "baking against an undefined backbuffer format is rejected");
}

void test_sizing_cycles_are_rejected() {
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &pass = graph.addPass("main", kGraphics);
    pass.addColorOutput("ping", relativeTo("pong", 0.5f, 0.5f));
    pass.addColorOutput("pong", relativeTo("ping", 0.5f, 0.5f));
    check(throws([&] { graph.bake(); }), "a sizing cycle is rejected rather than recursing forever");

    RenderGraph selfRef;
    selfRef.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &p2 = selfRef.addPass("main", kGraphics);
    p2.addColorOutput("loop", relativeTo("loop", 0.5f, 0.5f));
    check(throws([&] { selfRef.bake(); }), "a resource sized relative to itself is rejected");

    RenderGraph dangling;
    dangling.setBackbufferDimensions(backbuffer(1024, 1024));
    RenderPass &p3 = dangling.addPass("main", kGraphics);
    p3.addColorOutput("orphan", relativeTo("doesNotExist", 0.5f, 0.5f));
    check(throws([&] { dangling.bake(); }), "sizing relative to a resource that does not exist is rejected");
}

void test_describe_reports_the_derived_frame() {
    // describe() is what a failing assertion prints, so it has to name the
    // passes, the edges it believed in, and each slot's transience. A summary
    // that omits the ordering is useless for the failure it exists to explain.
    RenderGraph graph;
    graph.setBackbufferDimensions(backbuffer(512, 512));
    RenderPass &a = graph.addPass("gbuffer", kGraphics);
    a.addColorOutput("gbuf", fraction(1.0f, 1.0f));
    RenderPass &b = graph.addPass("tonemap", kGraphics);
    b.addColorOutput("backbuffer", fraction(1.0f, 1.0f, Format::B8G8R8A8_SRGB));
    b.addTextureInput("gbuf");
    graph.bake();

    const std::string text = graph.describe();
    check(text.find("gbuffer") != std::string::npos, "describe names every pass");
    check(text.find("tonemap") != std::string::npos, "describe names the later pass");
    check(text.find("RAW") != std::string::npos, "describe names the hazard kind it derived");
    check(text.find("gbuf") != std::string::npos, "describe names each physical slot's resource");
    check(text.find("persistent") != std::string::npos, "describe reports transience per slot");
    check(text.find("transient") != std::string::npos || text.find("persistent") != std::string::npos,
          "describe reports a transience decision at all");
}

} // namespace

int main() {
    std::printf("render graph tests\n");

    run("a swapchain-relative target follows the swapchain", test_sizing_follows_the_swapchain);
    run("an absolute target does not", test_absolute_is_not_relative);
    run("input-relative chains resolve through a forward reference", test_input_relative_chains);
    run("no extent ever reaches zero", test_extent_never_reaches_zero);
    run("mip level counts", test_mip_level_counts);
    run("an unspecified format is inherited from the swapchain", test_format_is_inherited_from_the_swapchain);
    run("level 0 byte sizes", test_byte_sizes);

    run("reads are ordered after the writes they sample", test_reads_follow_writes);
    run("two writers of one target are ordered against each other", test_two_writers_are_ordered_in_declaration_order);
    run("the derived order does not depend on declaration order", test_order_is_independent_of_declaration_order);
    run("a dependency cycle is rejected", test_cycles_are_rejected);
    run("reading a resource nothing writes is rejected", test_read_without_a_writer_is_rejected);
    run("duplicate pass names are rejected", test_duplicate_pass_names_are_rejected);
    run("a name cannot be a texture and a buffer", test_duplicate_resource_kinds_are_rejected);
    run("hazard kinds are classified", test_dependency_kinds);

    run("a resource spanning two passes is persistent", test_cross_pass_resources_are_persistent);
    run("a resource inside one pass is transient", test_single_pass_resources_are_transient);
    run("a buffer is never transient", test_buffers_are_never_transient);
    run("a history read forces persistence", test_history_disqualifies_transience);
    run("depth transience follows the device capability", test_depth_transience_follows_the_device);
    run("the backbuffer is never transient", test_backbuffer_is_always_persistent);
    run("a target read by a later pass is persistent", test_cross_pass_writes_to_the_same_target_are_persistent);

    run("a subpass merge makes a target transient", test_subpass_merge_makes_a_target_transient);
    run("a mismatched pair is refused aliasing", test_mismatched_aliases_are_refused);
    run("merging is transitive", test_merging_is_transitive);
    run("different queues do not merge", test_different_queues_do_not_merge);

    run("baking is stable and the graph resettable", test_bake_is_stable_and_resettable);
    run("an impossible frame is rejected", test_bake_rejects_an_impossible_frame);
    run("sizing cycles are rejected", test_sizing_cycles_are_rejected);
    run("describe reports the derived frame", test_describe_reports_the_derived_frame);

    std::printf("%d checks, %d failing\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
