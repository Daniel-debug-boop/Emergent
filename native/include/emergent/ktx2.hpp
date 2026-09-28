// A KTX2 reader.
//
// ## Why this exists
//
// The renderer binds three `sampler2DArray` maps and, until this, filled them
// with a neutral grey so the scene would light correctly while nothing was
// textured. The textures exist: 26 CC0 Poly Haven materials, baked to 78 PNGs.
// Getting them into Vulkan needs a KTX2 or KTX1 container reader, and the
// obvious one is KTX-Software — a large C++ project with its own build system
// and a `libktx` that drags in its own zlib, zstd, Basis and ASTC decoders.
//
// For three uncompressed-RGBA8 arrays that is a lot of dependency to read a
// four-field header. So this reads the container directly and links the libzstd
// the project already vendors.
//
// ## What is and is not supported
//
// Supported: 2D array textures, vkFormat R8G8B8A8_UNORM and _SRGB, a single
// mip chain, supercompression scheme 0 (none) and 2 (Zstandard). That is
// exactly what `tools/assets/ktx2.mjs` writes and exactly what the renderer
// binds, and it is checked rather than assumed: every unsupported field is
// rejected by name at load time, so a Basis-compressed or cubemap file produces
// "unsupported" instead of garbage texels.
//
// Not supported: Basis Universal (ETC1S/UASTC), which needs the Basis
// transcoder, and cubemaps and 3D textures, which nothing here produces.
//
// ## What is NOT claimed
//
// Nothing here has been validated against KTX-Software, `ktx2check`, or the
// Khronos KTX parser, because none is available in this environment. What *is*
// tested is that this reader and `tools/assets/ktx2.mjs` agree exactly, on a
// fixture committed to the tree, and that every field the reader trusts is
// bounds-checked before use. See docs/ENGINE_VERIFICATION.md.
//
// The defensive style is not decoration. A container format whose header
// contains byte offsets is exactly the kind of thing that reads out of bounds
// on a truncated file, and a GPU upload of out-of-bounds bytes is a crash on
// the machine with the least memory rather than an error.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace emergent {

/** Vulkan format values this reader accepts. */
inline constexpr uint32_t kVkFormatR8G8B8A8Unorm = 37;
inline constexpr uint32_t kVkFormatR8G8B8A8Srgb = 43;

/** KTX2 supercompression schemes. */
inline constexpr uint32_t kSupercompressionNone = 0;
inline constexpr uint32_t kSupercompressionZstd = 2;

/** One mip level of one image, decoded to raw texels. */
struct Ktx2Level {
    uint32_t width = 0;
    uint32_t height = 0;
    /** `layers * width * height * 4` bytes, layer 0 first, tightly packed. */
    std::vector<uint8_t> pixels;
};

/** A parsed, fully decoded KTX2 file. */
struct Ktx2Image {
    uint32_t vkFormat = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 0;
    uint32_t levelCount = 0;
    uint32_t supercompressionScheme = 0;

    /** Level 0 first, matching the file's own level order. */
    std::vector<Ktx2Level> levels;

    /** Every key/value pair, decoded. The writer's own provenance lives here. */
    std::vector<std::pair<std::string, std::string>> metadata;

    bool srgb() const { return vkFormat == kVkFormatR8G8B8A8Srgb; }
    /** Bytes per texel. Only 4 is reachable, because only RGBA8 is supported. */
    uint32_t bytesPerTexel() const { return 4; }

    const std::string *findMetadata(const std::string &key) const;
};

/**
 * Read and decode a KTX2 file.
 *
 * Every level is decompressed on load rather than lazily. A 26-layer array is
 * tens of megabytes, and a lazily-decoded level that turns out to be truncated
 * fails halfway through a frame upload instead of at load, which is the worst
 * time to find out.
 *
 * @param path   File to read.
 * @param out    Populated on success.
 * @param error  A sentence naming the specific field that was wrong.
 * @return false on any failure, including every unsupported case.
 */
bool loadKtx2(const std::string &path, Ktx2Image &out, std::string &error);

/** As above, for bytes already in memory. Used by the tests. */
bool parseKtx2(const uint8_t *data, size_t size, Ktx2Image &out, std::string &error);

/**
 * Validate only the header and the block offsets, without decoding any level.
 *
 * For checking a large asset cheaply, and for reporting *why* a file is
 * unusable without waiting for a full decompress. It performs the same
 * structural checks `parseKtx2` does, so a file that passes here can still fail
 * on its level data, but never the other way round.
 */
bool inspectKtx2(const uint8_t *data, size_t size, Ktx2Image &out, std::string &error);

/**
 * One zstd frame, decompressed to exactly `expectedBytes`.
 *
 * Exposed for the tests, which check it against a known frame, and used
 * internally. `expectedBytes` is passed in rather than discovered because a
 * zstd frame's decompressed size is not recorded in a way that can be trusted
 * without a dictionary; taking the caller's word and verifying the frame ends
 * exactly there is both simpler and stricter.
 */
bool zstdDecode(const uint8_t *src, size_t srcSize, uint8_t *dst, size_t expectedBytes,
                std::string &error);

}  // namespace emergent
