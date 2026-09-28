// KTX2 reader tests.
//
// The reader is the only thing standing between 78 CC0 PNGs and the GPU, and it
// is the part of the texture pipeline that cannot be checked by looking at the
// result. A decoder that returns plausible bytes for a corrupt file produces
// walls with the wrong bricks and no error anywhere, which is the failure mode
// this suite exists to make impossible.
//
// The central test is a byte-exact round trip: a real KTX2 file, committed to
// the tree, written by the same writer the packer uses, containing a pattern
// chosen so that any transposition, stride mistake or channel swap shows up.
// Not a gradient, and not a solid colour — a flat image decodes correctly under
// a surprising number of wrong strides.
//
// The negative tests are the other half. Every field the reader trusts is fed a
// value that should be rejected, because a container format whose header is full
// of byte offsets is a format that reads out of bounds on a truncated file.

#include "emergent/ktx2.hpp"

#include <cstdlib>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
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

/**
 * Paths are absolute, taken from the build system.
 *
 * A relative path here works when the test is run from the repository root and
 * fails under ctest, which runs it from the build directory. That is not a
 * cosmetic difference: the fixture test then reports "the committed fixture
 * decodes: could not open ..." and the byte-exactness check silently degrades
 * into a no-op that passes for the wrong reason.
 */
#ifndef EMERGENT_SOURCE_DIR
#define EMERGENT_SOURCE_DIR "."
#endif
#ifndef EMERGENT_NATIVE_ASSET_DIR
#define EMERGENT_NATIVE_ASSET_DIR "build/native-assets"
#endif

const std::string kFixture = std::string(EMERGENT_SOURCE_DIR) + "/native/tests/fixtures/pattern.ktx2";
const std::string kAssetDir = EMERGENT_NATIVE_ASSET_DIR;

std::vector<uint8_t> readFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamsize n = f.tellg();
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    f.seekg(0);
    if (n > 0) f.read(reinterpret_cast<char *>(bytes.data()), n);
    return bytes;
}

/** A deterministic pattern with a per-channel signature. */
uint8_t patternByte(uint32_t x, uint32_t y, uint32_t layer, uint32_t channel) {
    // R and G are separable, B is a hash, A is a ramp. A stride error, a
    // layer-order swap and a channel swap each break a different one of these.
    return static_cast<uint8_t>((x * 7 + y * 3 + layer * 11 + channel * 37 + ((x * y) ^ 0x5a)) & 0xff);
}

// ---------------------------------------------------------------------------

void test_a_real_file_decodes_byte_exactly() {
    Ktx2Image image;
    std::string error;
    const bool ok = loadKtx2(kFixture, image, error);
    check(ok, std::string("the committed fixture decodes: ") + error);
    if (!ok) return;

    check(image.width == 8, "width comes from the header");
    check(image.height == 8, "height comes from the header");
    check(image.layers == 3, "layer count comes from the header");
    check(image.levelCount == 4, "the mip chain length comes from the header");
    check(image.srgb(), "the fixture is sRGB, as albedo is");
    check(image.supercompressionScheme == kSupercompressionZstd, "and zstd-supercompressed");

    // Level dimensions follow the KTX2 floor rule.
    check(image.levels[0].width == 8 && image.levels[0].height == 8, "level 0 is 8x8");
    check(image.levels[1].width == 4 && image.levels[1].height == 4, "level 1 is 4x4");
    check(image.levels[2].width == 2 && image.levels[2].height == 2, "level 2 is 2x2");
    check(image.levels[3].width == 1 && image.levels[3].height == 1, "level 3 is 1x1");

    // The payload. Every level, every layer, every texel, every channel.
    for (uint32_t level = 0; level < image.levelCount; ++level) {
        const Ktx2Level &l = image.levels[level];
        check(l.pixels.size() == static_cast<size_t>(l.width) * l.height * image.layers * 4,
              "level " + std::to_string(level) + " has exactly the expected byte count");
        for (uint32_t layer = 0; layer < image.layers; ++layer) {
            for (uint32_t y = 0; y < l.height; ++y) {
                for (uint32_t x = 0; x < l.width; ++x) {
                    const size_t base =
                        ((static_cast<size_t>(layer) * l.height + y) * l.width + x) * 4;
                    for (uint32_t c = 0; c < 4; ++c) {
                        const uint8_t got = l.pixels[base + c];
                        const uint8_t want = patternByte(x, y, layer, c);
                        if (got != want) {
                            check(false, "level " + std::to_string(level) + " layer " +
                                             std::to_string(layer) + " texel (" +
                                             std::to_string(x) + "," + std::to_string(y) +
                                             ") channel " + std::to_string(c) + " is " +
                                             std::to_string(got) + ", expected " +
                                             std::to_string(want));
                            return;
                        }
                    }
                }
            }
        }
    }
    check(true, "every texel of every level and layer matches the source pattern exactly");
}

void test_metadata_survives_the_round_trip() {
    Ktx2Image image;
    std::string error;
    if (!loadKtx2(kFixture, image, error)) {
        check(false, "the fixture decodes");
        return;
    }
    const std::string *orientation = image.findMetadata("KTXorientation");
    check(orientation != nullptr, "KTXorientation is present");
    if (orientation) {
        // "rd" is right then down. A reader that gets this wrong flips every
        // normal map vertically, which looks like a lighting bug and is not one.
        check(*orientation == "rd", "KTXorientation is rd, so nothing is flipped");
    }
    const std::string *licence = image.findMetadata("EMERGENTlicense");
    check(licence != nullptr, "the licence travels inside the container");
    if (licence) check(*licence == "CC0", "and it is CC0");

    check(image.findMetadata("no-such-key") == nullptr, "an absent key returns null, not garbage");
}

void test_the_structural_header_is_readable_without_decoding() {
    // The cheap path, for checking a large asset before committing to a full
    // decompress. It must accept exactly what the full path accepts.
    const std::vector<uint8_t> bytes = readFile(kFixture);
    check(!bytes.empty(), "the fixture is on disk");
    if (bytes.empty()) return;

    Ktx2Image image;
    std::string error;
    check(inspectKtx2(bytes.data(), bytes.size(), image, error),
          std::string("inspect accepts a good file: ") + error);
    check(image.width == 8 && image.layers == 3, "and reports the same dimensions");
    check(image.levels.empty(), "without decoding any level");
}

void test_malformed_files_are_rejected_by_name() {
    const std::vector<uint8_t> good = readFile(kFixture);
    if (good.empty()) {
        check(false, "the fixture is on disk");
        return;
    }

    auto rejects = [&](std::vector<uint8_t> bytes, const char *why) {
        Ktx2Image image;
        std::string error;
        const bool accepted = parseKtx2(bytes.data(), bytes.size(), image, error);
        check(!accepted, std::string("rejected: ") + why);
        if (accepted) {
            check(false, std::string("...and it said so, not silence: ") + why);
        } else {
            // A rejection with no explanation is nearly as bad as an acceptance:
            // it tells the reader nothing about which field was wrong.
            check(!error.empty(), std::string("...with a reason: ") + why);
        }
    };

    rejects({}, "an empty file");
    rejects(std::vector<uint8_t>(40, 0), "a file shorter than the header");

    {
        std::vector<uint8_t> b = good;
        b[0] ^= 0xff;
        rejects(b, "a broken identifier");
    }
    {
        // vkFormat, at byte 12.
        std::vector<uint8_t> b = good;
        b[12] = 99;
        rejects(b, "an unsupported vkFormat");
    }
    {
        // Supercompression scheme, at byte 44: 1 is BasisLZ.
        std::vector<uint8_t> b = good;
        b[44] = 1;
        rejects(b, "BasisLZ supercompression, which needs a transcoder");
    }
    {
        // faceCount, at byte 36: 6 is a cubemap.
        std::vector<uint8_t> b = good;
        b[36] = 6;
        rejects(b, "a cubemap");
    }
    {
        // pixelDepth, at byte 28: a 3D texture.
        std::vector<uint8_t> b = good;
        b[28] = 1;
        rejects(b, "a 3D texture");
    }
    {
        // A level offset pointing past the end. The level index starts at 80.
        std::vector<uint8_t> b = good;
        b[80] = 0xff; b[81] = 0xff; b[82] = 0xff; b[83] = 0xff;
        rejects(b, "a level offset past the end of the file");
    }
    {
        // levelCount, at byte 40: more levels than the index can hold.
        std::vector<uint8_t> b = good;
        b[40] = 200;
        rejects(b, "an implausible level count");
    }
    {
        // The DFD offset, at byte 48, pushed beyond the file.
        std::vector<uint8_t> b = good;
        b[48] = 0xfe; b[49] = 0xff; b[50] = 0xff; b[51] = 0x7f;
        rejects(b, "a data format descriptor outside the file");
    }
    {
        // Truncated in the middle of the level data: the header still parses, so
        // this is the case that would read out of bounds without the checks.
        std::vector<uint8_t> b(good.begin(), good.begin() + static_cast<long>(good.size() / 2));
        rejects(b, "a file truncated in the middle of its level data");
    }
}

void test_corruption_inside_a_level_is_caught() {
    // The one that matters, and the one that was silently passing.
    //
    // Corrupting a frame *header* is caught by zstd immediately. Corrupting the
    // entropy-coded body is not, unless the frame carries a content checksum:
    // zstd decodes a damaged block into wrong bytes and reports success. So a
    // texture file can be corrupt in the middle, load without complaint, and
    // put the wrong bricks on every wall in the game.
    //
    // The writer therefore sets ZSTD_c_checksumFlag. These checks are what stop
    // that being silently undone by someone removing it to save four bytes per
    // level.
    const std::vector<uint8_t> good = readFile(kFixture);
    if (good.empty()) {
        check(false, "the fixture is on disk");
        return;
    }
    // Read the real offset out of the level index rather than assuming it. The
    // index is at byte 80, 24 bytes per level, and level 0's payload does not
    // start there: the data format descriptor and the key/value block sit in
    // between. Guessing produced a test that was corrupting the descriptor and
    // passing for the wrong reason.
    auto read64 = [&good](size_t at) {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | good[at + static_cast<size_t>(i)];
        return v;
    };
    const size_t level0Offset = static_cast<size_t>(read64(80));
    const size_t level0Length = static_cast<size_t>(read64(88));
    check(level0Offset >= 80u + static_cast<size_t>(good[40]) * 24u, "level 0 starts after the level index");
    check(level0Offset + level0Length <= good.size(), "level 0 lies inside the file");
    check(level0Length > 64, "level 0 is long enough to corrupt in its middle");

    // Three positions: just inside the frame header, deep in the entropy-coded
    // body, and near the end. All three must be rejected.
    for (size_t into : std::vector<size_t>{4, level0Length / 2, level0Length - 8}) {
        std::vector<uint8_t> b = good;
        b[level0Offset + into] ^= 0xff;
        b[level0Offset + into + 1] ^= 0xff;
        Ktx2Image image;
        std::string error;
        const bool accepted = parseKtx2(b.data(), b.size(), image, error);
        check(!accepted, "corruption " + std::to_string(into) +
                             " bytes into level 0 is rejected, not decoded");
        if (!accepted) {
            check(!error.empty(),
                  "...with a reason, at " + std::to_string(into));
        }
    }

    // And the mid-body case specifically must be caught by the checksum rather
    // than by luck. This is the assertion that fails if someone drops
    // ZSTD_c_checksumFlag from the writer to save four bytes per level.
    {
        std::vector<uint8_t> b = good;
        const size_t mid = level0Offset + level0Length / 2;
        b[mid] ^= 0xff;
        b[mid + 1] ^= 0xff;
        Ktx2Image image;
        std::string error;
        const bool accepted = parseKtx2(b.data(), b.size(), image, error);
        check(!accepted, "a mid-body corruption is caught");
        check(error.find("checksum") != std::string::npos ||
                  error.find("corruption") != std::string::npos,
              "...by the content checksum, not incidentally: " + error);
    }
}

void test_a_truncated_zstd_frame_is_not_silently_accepted() {
    // The specific failure this reader exists to prevent: a frame that decodes
    // to fewer bytes than the level needs, reported as success. The level
    // would then be uploaded with its tail still zero.
    Ktx2Image image;
    std::string error;
    if (!loadKtx2(kFixture, image, error)) {
        check(false, "the fixture decodes");
        return;
    }
    // Take level 0's stored bytes and hand them to the decoder expecting more
    // than they can produce.
    const std::vector<uint8_t> bytes = readFile(kFixture);
    const uint32_t level0Bytes = 8 * 8 * 3 * 4;
    const uint8_t *level0 = bytes.data() + (bytes.size() - level0Bytes);
    std::vector<uint8_t> tooSmall(level0Bytes / 2);
    std::memcpy(tooSmall.data(), level0, tooSmall.size());
    std::vector<uint8_t> out(level0Bytes);
    std::string zstdError;
    const bool ok = zstdDecode(tooSmall.data(), tooSmall.size(), out.data(), out.size(), zstdError);
    check(!ok, "a short zstd frame is rejected rather than half-decoded");
    check(!zstdError.empty(), "and says why");
}

void test_the_real_asset_packs_are_structurally_sound() {
    // The three files the packer actually produced, if they have been built.
    // Checked structurally and cheaply: a full decode of 50 MB in a unit test
    // would dominate the suite's runtime, and the byte-exactness of the
    // decoder is already established by the fixture above.
    const char *names[] = {"albedo.ktx2", "normal.ktx2", "arm.ktx2"};
    for (const char *name : names) {
        const std::string path = kAssetDir + "/" + name;
        Ktx2Image image;
        std::string error;
        if (!loadKtx2(path, image, error)) {
            // Not built in this checkout is not a failure; a malformed one is.
            if (error.find("could not open") != std::string::npos) {
                std::printf("  skip %s (not built; run npm run assets:textures)\n", name);
                g_checks++;
                continue;
            }
            check(false, std::string(name) + " is structurally sound: " + error);
            continue;
        }
        check(image.layers == 26, std::string(name) + " has all 26 material layers");
        check(image.width == image.height, std::string(name) + " is square");
        check(image.levelCount >= 2, std::string(name) + " has a mip chain");
        const std::string *licence = image.findMetadata("EMERGENTlicense");
        check(licence != nullptr, std::string(name) + " carries its licence in the container");
    }
    // Albedo is sRGB and the other two are not. Baking a normal map as sRGB is
    // the classic "why is the lighting wrong and nobody can say" bug.
    Ktx2Image albedo, normal, arm;
    std::string error;
    if (loadKtx2(kAssetDir + "/albedo.ktx2", albedo, error) &&
        loadKtx2(kAssetDir + "/normal.ktx2", normal, error) &&
        loadKtx2(kAssetDir + "/arm.ktx2", arm, error)) {
        check(albedo.srgb(), "albedo is sRGB");
        check(!normal.srgb(), "normal is linear: it is data, not colour");
        check(!arm.srgb(), "arm is linear: roughness and metalness are not colours");
        check(albedo.width == normal.width && normal.width == arm.width,
              "all three maps are the same size, so a layer index means the same material in each");
    }
}

}  // namespace

int main() {
    std::printf("emergent ktx2 tests\n");
    run("a real file decodes byte exactly", test_a_real_file_decodes_byte_exactly);
    run("metadata survives the round trip", test_metadata_survives_the_round_trip);
    run("the structural header is readable without decoding", test_the_structural_header_is_readable_without_decoding);
    run("malformed files are rejected by name", test_malformed_files_are_rejected_by_name);
    run("corruption inside a level is caught", test_corruption_inside_a_level_is_caught);
    run("a truncated zstd frame is not silently accepted", test_a_truncated_zstd_frame_is_not_silently_accepted);
    run("the real asset packs are structurally sound", test_the_real_asset_packs_are_structurally_sound);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
