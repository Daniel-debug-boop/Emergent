#include "emergent/ktx2.hpp"

#include <zstd.h>

#include <cstring>
#include <fstream>

namespace emergent {
namespace {

constexpr size_t kHeaderBytes = 80;
constexpr size_t kLevelIndexEntryBytes = 24;

/** The 12-byte signature: «KTX 20» with the framing bytes that make it a magic. */
constexpr uint8_t kIdentifier[12] = {0xab, 0x4b, 0x54, 0x58, 0x20, 0x32,
                                      0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};

uint32_t readU32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readU64(const uint8_t *p) {
    return static_cast<uint64_t>(readU32(p)) | (static_cast<uint64_t>(readU32(p + 4)) << 32);
}

/** Level dimensions, following the KTX2 floor rule with a minimum of one. */
void levelDimensions(uint32_t width, uint32_t height, uint32_t level, uint32_t &outW, uint32_t &outH) {
    const uint64_t w = width >> level;
    const uint64_t h = height >> level;
    outW = static_cast<uint32_t>(w == 0 ? 1 : w);
    outH = static_cast<uint32_t>(h == 0 ? 1 : h);
}

struct LevelEntry {
    uint64_t byteOffset = 0;
    uint64_t byteLength = 0;
    uint64_t uncompressedByteLength = 0;
};

/**
 * Parse the header, the level index and the metadata blocks.
 *
 * Every field that becomes a pointer or a length is checked against the file
 * size before it is used, and the metadata blocks are checked against each
 * other as well, because a file can have individually-plausible offsets that
 * together describe overlapping regions.
 */
bool parseStructure(const uint8_t *data, size_t size, Ktx2Image &out,
                    std::vector<LevelEntry> &levels, std::string &error, bool decodeLevels) {
    if (size < kHeaderBytes) {
        error = "file is shorter than the 80-byte header";
        return false;
    }
    if (std::memcmp(data, kIdentifier, sizeof(kIdentifier)) != 0) {
        error = "not a KTX2 file (identifier mismatch)";
        return false;
    }

    out.vkFormat = readU32(data + 12);
    const uint32_t typeSize = readU32(data + 16);
    out.width = readU32(data + 20);
    out.height = readU32(data + 24);
    const uint32_t pixelDepth = readU32(data + 28);
    out.layers = readU32(data + 32);
    const uint32_t faceCount = readU32(data + 36);
    out.levelCount = readU32(data + 40);
    out.supercompressionScheme = readU32(data + 44);
    const uint32_t dfdOffset = readU32(data + 48);
    const uint32_t dfdLength = readU32(data + 52);
    const uint32_t kvdOffset = readU32(data + 56);
    const uint32_t kvdLength = readU32(data + 60);
    const uint64_t sgdOffset = readU64(data + 64);
    const uint64_t sgdLength = readU64(data + 72);

    // -- the fields this reader will not guess at --------------------------
    if (out.vkFormat != kVkFormatR8G8B8A8Unorm && out.vkFormat != kVkFormatR8G8B8A8Srgb) {
        error = "unsupported vkFormat " + std::to_string(out.vkFormat) +
                " (only R8G8B8A8_UNORM and _SRGB are read here; a Basis-compressed "
                "file needs the Basis transcoder, not a container reader)";
        return false;
    }
    if (typeSize != 1) {
        error = "unsupported typeSize " + std::to_string(typeSize) + " (only 1-byte components)";
        return false;
    }
    if (pixelDepth != 0) {
        error = "unsupported pixelDepth " + std::to_string(pixelDepth) + " (3D textures are not read)";
        return false;
    }
    if (faceCount != 1) {
        error = "unsupported faceCount " + std::to_string(faceCount) + " (cubemaps are not read)";
        return false;
    }
    if (out.supercompressionScheme != kSupercompressionNone &&
        out.supercompressionScheme != kSupercompressionZstd) {
        error = "unsupported supercompressionScheme " +
                std::to_string(out.supercompressionScheme) +
                " (only 0 none and 2 zstd; scheme 1 is BasisLZ, which needs a transcoder)";
        return false;
    }
    if (out.width == 0 || out.height == 0 || out.layers == 0) {
        error = "zero-sized image (" + std::to_string(out.width) + "x" + std::to_string(out.height) +
                " x " + std::to_string(out.layers) + " layers)";
        return false;
    }
    if (out.levelCount == 0 || out.levelCount > 16) {
        error = "implausible levelCount " + std::to_string(out.levelCount);
        return false;
    }

    // -- level index -------------------------------------------------------
    const size_t indexBytes = static_cast<size_t>(out.levelCount) * kLevelIndexEntryBytes;
    if (kHeaderBytes + indexBytes > size) {
        error = "the level index runs past the end of the file";
        return false;
    }
    levels.clear();
    levels.reserve(out.levelCount);
    for (uint32_t i = 0; i < out.levelCount; ++i) {
        const uint8_t *p = data + kHeaderBytes + static_cast<size_t>(i) * kLevelIndexEntryBytes;
        LevelEntry e;
        e.byteOffset = readU64(p);
        e.byteLength = readU64(p + 8);
        e.uncompressedByteLength = readU64(p + 16);
        // A 64-bit offset compared against a size_t is the comparison most likely
        // to wrap on a 32-bit build, so it is done in 64-bit explicitly.
        if (e.byteOffset > static_cast<uint64_t>(size) || e.byteLength > static_cast<uint64_t>(size) ||
            e.byteOffset + e.byteLength > static_cast<uint64_t>(size)) {
            error = "level " + std::to_string(i) + " lies outside the file (offset " +
                    std::to_string(e.byteOffset) + ", length " + std::to_string(e.byteLength) +
                    ", file " + std::to_string(size) + ")";
            return false;
        }
        levels.push_back(e);
    }

    // -- metadata blocks ---------------------------------------------------
    // Each is checked for containment, and then for ordering, so a file whose
    // blocks claim to be inside the file but overlap the level data is rejected
    // rather than quietly believed.
    struct Block {
        const char *name;
        uint64_t offset;
        uint64_t length;
        uint32_t width;  // 32-bit fields come from the header
    };
    const Block blocks[] = {
        {"dfd", dfdOffset, dfdLength, 32},
        {"kvd", kvdOffset, kvdLength, 32},
        {"sgd", sgdOffset, sgdLength, 64},
    };
    for (const Block &b : blocks) {
        const uint64_t len = b.width == 32 ? static_cast<uint64_t>(b.length) : b.length;
        if (b.offset > static_cast<uint64_t>(size) || len > static_cast<uint64_t>(size) ||
            b.offset + len > static_cast<uint64_t>(size)) {
            error = std::string("the ") + b.name + " block lies outside the file (offset " +
                    std::to_string(b.offset) + ", length " + std::to_string(len) + ", file " +
                    std::to_string(size) + ")";
            return false;
        }
    }
    for (size_t i = 0; i + 1 < sizeof(blocks) / sizeof(blocks[0]); ++i) {
        const uint64_t aEnd = blocks[i].offset + (blocks[i].width == 32 ? blocks[i].length : blocks[i].length);
        const uint64_t bStart = blocks[i + 1].offset;
        if (blocks[i].length != 0 && blocks[i + 1].length != 0 && aEnd > bStart) {
            error = std::string("the ") + blocks[i].name + " and " + blocks[i + 1].name +
                    " blocks overlap";
            return false;
        }
    }

    // A zstd file must carry its compression level, because the level is what a
    // reader needs in order to re-compress a level after decoding it. A missing
    // SGD is not fatal here — this reader only decompresses — but it is worth
    // naming, because the writer is the thing that is supposed to emit it.
    if (out.supercompressionScheme == kSupercompressionZstd && sgdLength != 4) {
        error = "a zstd-supercompressed file must have a 4-byte supercompression global data block, got " +
                std::to_string(sgdLength);
        return false;
    }

    // -- key/value metadata ------------------------------------------------
    if (kvdLength > 0) {
        uint64_t cursor = kvdOffset;
        const uint64_t kvdEnd = kvdOffset + kvdLength;
        while (cursor + 4 <= kvdEnd) {
            const uint32_t entryLength = readU32(data + cursor);
            cursor += 4;
            if (entryLength == 0) {
                error = "a zero-length key/value entry";
                return false;
            }
            if (cursor + entryLength > kvdEnd) {
                error = "a key/value entry runs past the end of the block";
                return false;
            }
            const uint8_t *entry = data + cursor;
            // The key is NUL-terminated and must be inside the entry.
            size_t keyLength = 0;
            while (keyLength < entryLength && entry[keyLength] != 0) ++keyLength;
            if (keyLength >= entryLength) {
                error = "a key/value entry has an unterminated key";
                return false;
            }
            const std::string key(reinterpret_cast<const char *>(entry), keyLength);
            const size_t valueOffset = keyLength + 1;
            const size_t valueLength = entryLength - valueOffset;
            out.metadata.emplace_back(
                key, std::string(reinterpret_cast<const char *>(entry + valueOffset), valueLength));
            // Entries are padded to a four-byte boundary, and the length field
            // excludes that padding.
            cursor += (entryLength + 3) & ~static_cast<uint64_t>(3);
        }
    }

    if (!decodeLevels) return true;

    // -- level payloads ----------------------------------------------------
    out.levels.resize(out.levelCount);
    for (uint32_t i = 0; i < out.levelCount; ++i) {
        uint32_t w = 0, h = 0;
        levelDimensions(out.width, out.height, i, w, h);
        const uint64_t texels = static_cast<uint64_t>(w) * h * out.layers * out.bytesPerTexel();
        if (texels == 0 || texels > (1ull << 32)) {
            error = "level " + std::to_string(i) + " is implausibly large (" +
                    std::to_string(texels) + " bytes)";
            return false;
        }
        Ktx2Level &level = out.levels[i];
        level.width = w;
        level.height = h;
        level.pixels.resize(static_cast<size_t>(texels));

        const LevelEntry &e = levels[i];
        if (out.supercompressionScheme == kSupercompressionNone) {
            if (e.byteLength != texels) {
                error = "level " + std::to_string(i) + " is " + std::to_string(e.byteLength) +
                        " bytes but " + std::to_string(texels) + " were expected";
                return false;
            }
            std::memcpy(level.pixels.data(), data + e.byteOffset, static_cast<size_t>(e.byteLength));
        } else {
            if (e.uncompressedByteLength != texels) {
                error = "level " + std::to_string(i) + " declares " +
                        std::to_string(e.uncompressedByteLength) + " uncompressed bytes but " +
                        std::to_string(texels) + " were expected";
                return false;
            }
            std::string zstdError;
            if (!zstdDecode(data + e.byteOffset, static_cast<size_t>(e.byteLength),
                            level.pixels.data(), static_cast<size_t>(texels), zstdError)) {
                error = "level " + std::to_string(i) + ": " + zstdError;
                return false;
            }
        }
    }
    return true;
}

}  // namespace

const std::string *Ktx2Image::findMetadata(const std::string &key) const {
    for (const auto &entry : metadata) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

bool zstdDecode(const uint8_t *src, size_t srcSize, uint8_t *dst, size_t expectedBytes,
                std::string &error) {
    // The frame's decompressed size is not reliably discoverable without a
    // dictionary, so the caller supplies it and this verifies the frame ends
    // exactly there. ZSTD_decompressStream returning "frame complete" after
    // producing fewer bytes than expected is the case that matters: it means a
    // truncated or mismatched level, which must not be mistaken for success.
    ZSTD_DStream *stream = ZSTD_createDStream();
    if (stream == nullptr) {
        error = "could not create a zstd decompression stream";
        return false;
    }
    ZSTD_initDStream(stream);

    ZSTD_inBuffer input{src, srcSize, 0};
    ZSTD_outBuffer output{dst, expectedBytes, 0};
    const size_t result = ZSTD_decompressStream(stream, &output, &input);
    const bool complete = ZSTD_isError(result) == 0 && result == 0;
    ZSTD_freeDStream(stream);

    if (!complete) {
        if (ZSTD_isError(result)) {
            error = std::string("zstd: ") + ZSTD_getErrorName(result);
        } else if (output.pos != expectedBytes) {
            error = "zstd: frame produced " + std::to_string(output.pos) + " of " +
                    std::to_string(expectedBytes) + " expected bytes";
        } else {
            error = "zstd: frame did not end cleanly (trailing data)";
        }
        return false;
    }
    return true;
}

bool inspectKtx2(const uint8_t *data, size_t size, Ktx2Image &out, std::string &error) {
    std::vector<LevelEntry> levels;
    return parseStructure(data, size, out, levels, error, false);
}

bool parseKtx2(const uint8_t *data, size_t size, Ktx2Image &out, std::string &error) {
    std::vector<LevelEntry> levels;
    return parseStructure(data, size, out, levels, error, true);
}

bool loadKtx2(const std::string &path, Ktx2Image &out, std::string &error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "could not open " + path;
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size < 0) {
        error = "could not size " + path;
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (size > 0 && !file.read(reinterpret_cast<char *>(bytes.data()), size)) {
        error = "could not read " + path;
        return false;
    }
    file.close();
    return parseKtx2(bytes.data(), bytes.size(), out, error);
}

}  // namespace emergent
