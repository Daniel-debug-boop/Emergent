#include "emergent/asset_pack.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

// ZSTD_frameHeader and ZSTD_getFrameHeader live behind this gate. They are
// what let the reader inspect a frame's checksum flag and declared size
// before allocating anything, which is the difference between "a damaged pack
// fails to load" and "a damaged pack loads as garbage".
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace emergent {
namespace {

constexpr char kMagic[8] = {'E', 'Z', 'P', 'K', 'P', 'K', '0', '1'};
constexpr std::uint32_t kVersion = 1;
constexpr int kMinLevel = 1;
constexpr int kMaxLevel = 19;

// Serialised sizes. These are format constants, not sizeof() results: the
// header and the entry table are written field by field so padding and
// endianness cannot change the layout underneath a reader.
constexpr std::size_t kHeaderSize = 48;
constexpr std::size_t kEntrySize = 32;

// -- little-endian primitives ------------------------------------------------

void putU32(std::uint8_t *out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xffu);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
    out[2] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
    out[3] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
}

void putU64(std::uint8_t *out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu);
    }
}

std::uint32_t getU32(const std::uint8_t *in) {
    return static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) | (static_cast<std::uint32_t>(in[3]) << 24);
}

std::uint64_t getU64(const std::uint8_t *in) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
    return value;
}

void appendU32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    const std::size_t at = out.size();
    out.resize(at + 4);
    putU32(out.data() + at, value);
}

void appendU64(std::vector<std::uint8_t> &out, std::uint64_t value) {
    const std::size_t at = out.size();
    out.resize(at + 8);
    putU64(out.data() + at, value);
}

bool readWholeFile(const std::string &path, std::vector<std::uint8_t> &out, std::string &error) {
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        error = "cannot open '" + path + "': " + std::strerror(errno);
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        error = "cannot seek '" + path + "'";
        return false;
    }
    const long size = std::ftell(file);
    if (size < 0) {
        std::fclose(file);
        error = "cannot size '" + path + "'";
        return false;
    }
    std::rewind(file);
    out.resize(static_cast<std::size_t>(size));
    const std::size_t read = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    if (read != out.size()) {
        error = "short read on '" + path + "'";
        out.clear();
        return false;
    }
    return true;
}

bool writeWholeFile(const std::string &path, const std::vector<std::uint8_t> &bytes, std::string &error) {
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (!file) {
        error = "cannot create '" + path + "': " + std::strerror(errno);
        return false;
    }
    const std::size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), file);
    const bool flushed = std::fflush(file) == 0;
    std::fclose(file);
    if (written != bytes.size() || !flushed) {
        error = "short write on '" + path + "'";
        return false;
    }
    return true;
}

// One entry as stored in the index table.
struct Entry {
    std::string name;
    std::uint64_t dataOffset = 0;
    std::uint64_t compressedSize = 0;
    std::uint64_t originalSize = 0;
};

// Compresses `size` bytes into a complete zstd frame.
//
// ZSTD_c_checksumFlag is off by default (see ZSTD_c_checksumFlag in zstd.h), so
// the simple ZSTD_compress() API produces frames that decode happily after
// corruption. The checksum is explicitly requested here, which is the entire
// basis for the pack format's claim that a damaged pack fails to load.
bool compressFrame(ZSTD_CCtx *ctx, const void *data, std::size_t size, int level,
                   std::vector<std::uint8_t> &out, std::string &error) {
    if (ZSTD_isError(ZSTD_CCtx_setParameter(ctx, ZSTD_c_compressionLevel, level)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(ctx, ZSTD_c_checksumFlag, 1))) {
        error = "cannot configure the zstd compression context";
        out.clear();
        return false;
    }
    const void *source = data != nullptr ? data : "";
    const std::size_t bound = ZSTD_compressBound(size);
    out.resize(bound);
    const std::size_t written = ZSTD_compress2(ctx, out.data(), bound, source, size);
    if (ZSTD_isError(written)) {
        error = std::string("zstd compression failed: ") + ZSTD_getErrorName(written);
        out.clear();
        return false;
    }
    out.resize(written);
    return true;
}

// Decompresses one frame, refusing anything that is not a checksummed frame of
// exactly the expected size.
bool decompressFrame(ZSTD_DCtx *ctx, const std::uint8_t *data, std::size_t size,
                     std::size_t expected, std::vector<std::uint8_t> &out, std::string &error) {
    if (expected > (std::size_t{1} << 30)) {
        // A 1 GiB entry is already a bug or an attack, and zstd would allocate
        // the destination before validating anything about the frame.
        error = "entry claims an implausible uncompressed size";
        return false;
    }

    // The header is inspected before a single byte is written, so a frame that
    // is truncated, has no checksum, or disagrees with the index is rejected
    // without allocating its output.
    ZSTD_frameHeader header{};
    const std::size_t wanted = ZSTD_getFrameHeader(&header, data, size);
    if (ZSTD_isError(wanted)) {
        error = std::string("not a readable zstd frame: ") + ZSTD_getErrorName(wanted);
        return false;
    }
    if (wanted != 0) {
        error = "zstd frame header is truncated";
        return false;
    }
    if (header.frameType != ZSTD_frame) {
        error = "expected a zstd frame, found a skippable frame";
        return false;
    }
    if (header.checksumFlag == 0) {
        error = "zstd frame carries no content checksum; refusing an unverifiable entry";
        return false;
    }
    if (header.frameContentSize != static_cast<unsigned long long>(expected)) {
        error = "entry size disagrees with the index";
        return false;
    }

    out.resize(expected);
    const std::size_t written = ZSTD_decompressDCtx(ctx, out.data(), expected, data, size);
    if (ZSTD_isError(written)) {
        // Covers a truncated frame, a broken block and a failed content
        // checksum: all three are the failure this format is designed to catch.
        error = std::string("zstd decompression failed: ") + ZSTD_getErrorName(written);
        out.clear();
        return false;
    }
    if (written != expected) {
        error = "entry decompressed to a different size than the index recorded";
        out.clear();
        return false;
    }
    return true;
}

// Recursively collects regular files under `directory`, returning paths
// relative to it, sorted so a pack built twice from the same tree is
// byte-for-byte reproducible.
bool collectFiles(const std::string &directory, const std::string &relative,
                  std::vector<std::string> &out, std::string &error) {
    const std::string full = relative.empty() ? directory : directory + "/" + relative;
    DIR *dir = ::opendir(full.c_str());
    if (!dir) {
        error = "cannot list '" + full + "': " + std::strerror(errno);
        return false;
    }
    while (const dirent *entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        const std::string child = relative.empty() ? name : relative + "/" + name;
        const std::string childFull = directory + "/" + child;

        struct stat info {};
        if (::stat(childFull.c_str(), &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            if (!collectFiles(directory, child, out, error)) {
                ::closedir(dir);
                return false;
            }
        } else if (S_ISREG(info.st_mode)) {
            out.push_back(child);
        }
    }
    ::closedir(dir);
    std::sort(out.begin(), out.end());
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// AssetWriter
// ---------------------------------------------------------------------------

struct AssetWriter::Impl {
    std::string path;
    std::string error;
    std::vector<Entry> entries;
    std::unordered_set<std::string> names;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> tail;
    // One compression context for the life of the writer: rebuilding it per
    // entry would throw away the window and level history zstd keeps between
    // calls, which is most of what makes repeated frames cheap.
    ZSTD_CCtx *cctx = nullptr;
    std::uint64_t originalBytes = 0;
    int level = 9;
    bool open = false;

    void fail(std::string message) {
        if (error.empty()) error = std::move(message);
    }
    ~Impl() {
        if (cctx) ZSTD_freeCCtx(cctx);
    }
};

AssetWriter::AssetWriter() : impl_(std::make_unique<Impl>()) {}
AssetWriter::~AssetWriter() { close(); }
AssetWriter::AssetWriter(AssetWriter&&) noexcept = default;
AssetWriter& AssetWriter::operator=(AssetWriter&&) noexcept = default;

bool AssetWriter::open(const std::string &path, int level) {
    if (impl_->open) {
        impl_->fail("the writer is already open; close() it first");
        return false;
    }
    if (level < kMinLevel || level > kMaxLevel) {
        impl_->fail("compression level must be between 1 and 19");
        return false;
    }
    impl_->path = path;
    impl_->level = level;
    if (!impl_->cctx) impl_->cctx = ZSTD_createCCtx();
    if (!impl_->cctx) {
        impl_->fail("cannot create a zstd compression context");
        return false;
    }
    impl_->entries.clear();
    impl_->names.clear();
    impl_->bytes.assign(kHeaderSize, 0); // header placeholder
    impl_->originalBytes = 0;
    impl_->error.clear();
    impl_->open = true;
    return true;
}

bool AssetWriter::isOpen() const noexcept { return impl_ && impl_->open; }

const std::string &AssetWriter::path() const noexcept { return impl_->path; }

std::size_t AssetWriter::entryCount() const noexcept {
    return impl_ ? impl_->entries.size() : 0;
}

std::uint64_t AssetWriter::originalBytes() const noexcept { return impl_ ? impl_->originalBytes : 0; }

std::uint64_t AssetWriter::storedBytes() const noexcept { return impl_ ? impl_->bytes.size() : 0; }

const std::string &AssetWriter::error() const noexcept { return impl_->error; }

bool AssetWriter::add(std::string_view name, const void *data, std::size_t size) {
    if (!impl_->open) {
        impl_->fail("the writer is not open");
        return false;
    }
    if (name.empty()) {
        impl_->fail("an entry name cannot be empty");
        return false;
    }
    if (data == nullptr && size != 0) {
        impl_->fail("a null buffer cannot have a non-zero size");
        return false;
    }
    const std::string key(name);
    if (!impl_->names.insert(key).second) {
        impl_->fail("duplicate entry name '" + key + "'");
        return false;
    }

    Entry entry;
    entry.name = key;
    entry.dataOffset = impl_->bytes.size();
    entry.originalSize = size;
    if (!compressFrame(impl_->cctx, data, size, impl_->level, impl_->tail, impl_->error)) {
        impl_->names.erase(key);
        return false;
    }
    entry.compressedSize = impl_->tail.size();
    impl_->bytes.insert(impl_->bytes.end(), impl_->tail.begin(), impl_->tail.end());
    impl_->tail.clear();
    impl_->originalBytes += size;
    impl_->entries.push_back(std::move(entry));
    return true;
}

bool AssetWriter::addText(std::string_view name, std::string_view text) {
    return add(name, text.data(), text.size());
}

bool AssetWriter::addFile(std::string_view name, const std::string &sourcePath) {
    if (!impl_->open) {
        impl_->fail("the writer is not open");
        return false;
    }
    std::vector<std::uint8_t> contents;
    if (!readWholeFile(sourcePath, contents, impl_->error)) return false;
    return add(name, contents.data(), contents.size());
}

bool AssetWriter::addDirectory(std::string_view prefix, const std::string &sourceDirectory) {
    if (!impl_->open) {
        impl_->fail("the writer is not open");
        return false;
    }
    std::vector<std::string> files;
    if (!collectFiles(sourceDirectory, "", files, impl_->error)) return false;
    for (const std::string &relative : files) {
        std::string name = relative;
        if (!prefix.empty()) name = std::string(prefix) + "/" + relative;
        if (!addFile(name, sourceDirectory + "/" + relative)) return false;
    }
    return true;
}

/**
 * Lays out the index and patches the header, producing the finished pack.
 *
 * The header is written first as a placeholder and rewritten here, so a pack
 * is never half-written from a reader's point of view: the index frame is only
 * ever appended once every payload is already in place.
 */
bool AssetWriter::close() {
    // Nothing to finalise. This is what makes close() idempotent, and what the
    // destructor relies on for a writer that was never opened.
    if (!impl_ || !impl_->open) return true;

    // Index payload: the entry table followed by the name blob.
    std::vector<std::uint8_t> index;
    index.reserve(impl_->entries.size() * kEntrySize);
    std::uint32_t nameCursor = 0;
    for (const Entry &entry : impl_->entries) {
        appendU32(index, nameCursor);
        appendU32(index, static_cast<std::uint32_t>(entry.name.size()));
        appendU64(index, entry.dataOffset);
        appendU64(index, entry.compressedSize);
        appendU64(index, entry.originalSize);
        nameCursor += static_cast<std::uint32_t>(entry.name.size());
    }
    for (const Entry &entry : impl_->entries) {
        index.insert(index.end(), entry.name.begin(), entry.name.end());
    }

    const std::uint64_t indexOffset = impl_->bytes.size();
    if (!compressFrame(impl_->cctx, index.data(), index.size(), impl_->level, impl_->tail,
                       impl_->error)) {
        impl_->open = false;
        return false;
    }
    impl_->bytes.insert(impl_->bytes.end(), impl_->tail.begin(), impl_->tail.end());

    std::uint8_t *header = impl_->bytes.data();
    std::memcpy(header, kMagic, sizeof(kMagic));
    putU32(header + 8, kVersion);
    putU32(header + 12, static_cast<std::uint32_t>(impl_->entries.size()));
    putU64(header + 16, indexOffset);
    putU64(header + 24, impl_->tail.size());
    putU64(header + 32, index.size());
    putU32(header + 40, static_cast<std::uint32_t>(kHeaderSize));
    putU32(header + 44, 0); // reserved

    impl_->tail.clear();
    impl_->tail.shrink_to_fit();

    const bool written = writeWholeFile(impl_->path, impl_->bytes, impl_->error);
    impl_->open = false;
    return written;
}

// ---------------------------------------------------------------------------
// AssetPack
// ---------------------------------------------------------------------------

struct AssetPack::Impl {
    std::string path;
    std::string error;
    std::vector<std::uint8_t> bytes;
    std::vector<Entry> entries;
    std::unordered_map<std::string, std::size_t> lookup;    ZSTD_DCtx *dctx = nullptr;
    bool open = false;

    void fail(std::string message) {
        if (error.empty()) error = std::move(message);
    }
    ~Impl() {
        if (dctx) ZSTD_freeDCtx(dctx);
    }
    const Entry *find(std::string_view name) const {
        const auto it = lookup.find(std::string(name));
        return it == lookup.end() ? nullptr : &entries[it->second];
    }
};

AssetPack::AssetPack() : impl_(std::make_unique<Impl>()) {}
AssetPack::~AssetPack() = default;
AssetPack::AssetPack(AssetPack&&) noexcept = default;
AssetPack& AssetPack::operator=(AssetPack&&) noexcept = default;

const std::string &AssetPack::path() const noexcept { return impl_->path; }
const std::string &AssetPack::error() const noexcept { return impl_->error; }
bool AssetPack::isOpen() const noexcept { return impl_ && impl_->open; }

void AssetPack::close() {
    impl_->bytes.clear();
    impl_->bytes.shrink_to_fit();
    impl_->entries.clear();
    impl_->lookup.clear();
    impl_->open = false;
}

/**
 * Validates the container before trusting any of it.
 *
 * Every offset and size in the header is range-checked against the actual file
 * length before it is used, so a hostile or truncated file cannot steer a read
 * outside the buffer. Only then is the index decompressed, and zstd's own
 * frame checksum is what proves the index itself is intact.
 */
bool AssetPack::open(const std::string &path) {
    close();
    impl_->path = path;
    impl_->error.clear();
    if (!impl_->dctx) impl_->dctx = ZSTD_createDCtx();
    if (!impl_->dctx) {
        impl_->fail("cannot create a zstd decompression context");
        return false;
    }

    if (!readWholeFile(path, impl_->bytes, impl_->error)) return false;
    if (impl_->bytes.size() < kHeaderSize) {
        impl_->fail("file is too small to be an asset pack");
        impl_->bytes.clear();
        return false;
    }

    const std::uint8_t *header = impl_->bytes.data();
    if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
        impl_->fail("not an EMERGENT asset pack");
        impl_->bytes.clear();
        return false;
    }
    if (getU32(header + 8) != kVersion) {
        impl_->fail("unsupported asset pack version");
        impl_->bytes.clear();
        return false;
    }
    if (getU32(header + 40) != kHeaderSize) {
        impl_->fail("unexpected asset pack header size");
        impl_->bytes.clear();
        return false;
    }

    const std::uint32_t entryCount = getU32(header + 12);
    const std::uint64_t indexOffset = getU64(header + 16);
    const std::uint64_t indexSize = getU64(header + 24);
    const std::uint64_t indexRawSize = getU64(header + 32);

    if (indexSize == 0 || indexRawSize < static_cast<std::uint64_t>(entryCount) * kEntrySize) {
        impl_->fail("asset pack index is inconsistent with its entry count");
        impl_->bytes.clear();
        return false;
    }
    // Subtraction rather than addition, so a hostile index cannot overflow the
    // range check by carrying the sum past UINT64_MAX.
    if (indexOffset < kHeaderSize || indexOffset > impl_->bytes.size() ||
        indexSize > impl_->bytes.size() - indexOffset) {
        impl_->fail("asset pack index lies outside the file");
        impl_->bytes.clear();
        return false;
    }

    std::vector<std::uint8_t> index;
    if (!decompressFrame(impl_->dctx, impl_->bytes.data() + indexOffset,
                         static_cast<std::size_t>(indexSize),
                         static_cast<std::size_t>(indexRawSize), index, impl_->error)) {
        impl_->bytes.clear();
        return false;
    }

    impl_->entries.clear();
    impl_->entries.reserve(entryCount);
    const std::size_t tableSize = static_cast<std::size_t>(entryCount) * kEntrySize;
    for (std::uint32_t i = 0; i < entryCount; ++i) {
        const std::uint8_t *record = index.data() + static_cast<std::size_t>(i) * kEntrySize;
        Entry entry;
        const std::uint32_t nameOffset = getU32(record);
        const std::uint32_t nameLength = getU32(record + 4);
        entry.dataOffset = getU64(record + 8);
        entry.compressedSize = getU64(record + 16);
        entry.originalSize = getU64(record + 24);

        if (static_cast<std::uint64_t>(nameOffset) + nameLength > index.size() - tableSize) {
            impl_->fail("asset pack entry name lies outside the index");
            impl_->bytes.clear();
            return false;
        }
        entry.name.assign(reinterpret_cast<const char *>(index.data() + tableSize + nameOffset), nameLength);

        // Payload range check, against the real file length and the start of
        // the index. Written as subtractions so a hostile index cannot wrap
        // the sum past UINT64_MAX and slip past the check.
        if (entry.dataOffset < kHeaderSize || entry.dataOffset > impl_->bytes.size() ||
            entry.compressedSize == 0 ||
            entry.compressedSize > impl_->bytes.size() - entry.dataOffset ||
            entry.compressedSize > indexOffset - entry.dataOffset) {
            impl_->fail("asset pack entry '" + entry.name + "' lies outside the payload area");
            impl_->bytes.clear();
            return false;
        }
        if (impl_->lookup.find(entry.name) != impl_->lookup.end()) {
            impl_->fail("asset pack contains duplicate entry '" + entry.name + "'");
            impl_->bytes.clear();
            return false;
        }
        impl_->lookup.emplace(entry.name, impl_->entries.size());
        impl_->entries.push_back(std::move(entry));
    }

    impl_->open = true;
    return true;
}

std::size_t AssetPack::entryCount() const noexcept { return impl_ ? impl_->entries.size() : 0; }

bool AssetPack::contains(std::string_view name) const {
    return isOpen() && impl_->find(name) != nullptr;
}

std::vector<std::string> AssetPack::names() const {
    std::vector<std::string> out;
    if (!isOpen()) return out;
    out.reserve(impl_->entries.size());
    for (const Entry &entry : impl_->entries) out.push_back(entry.name);
    return out;
}

std::size_t AssetPack::entrySize(std::string_view name) const {
    const Entry *entry = impl_ ? impl_->find(name) : nullptr;
    return entry ? static_cast<std::size_t>(entry->originalSize) : 0;
}

std::uint64_t AssetPack::originalBytes() const noexcept {
    if (!isOpen()) return 0;
    std::uint64_t total = 0;
    for (const Entry &entry : impl_->entries) total += entry.originalSize;
    return total;
}

std::uint64_t AssetPack::storedBytes() const noexcept { return isOpen() ? impl_->bytes.size() : 0; }

double AssetPack::compressionRatio() const noexcept {
    if (!isOpen()) return 0.0;
    const std::uint64_t original = originalBytes();
    if (original == 0) return 0.0;
    return static_cast<double>(impl_->bytes.size()) / static_cast<double>(original);
}

bool AssetPack::read(std::string_view name, std::vector<std::uint8_t> &out) const {
    if (!isOpen()) return false;
    const Entry *entry = impl_->find(name);
    if (!entry) return false;
    return decompressFrame(impl_->dctx, impl_->bytes.data() + entry->dataOffset,
                           static_cast<std::size_t>(entry->compressedSize),
                           static_cast<std::size_t>(entry->originalSize), out, impl_->error);
}

bool AssetPack::readText(std::string_view name, std::string &out) const {
    std::vector<std::uint8_t> bytes;
    if (!read(name, bytes)) return false;
    out.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    return true;
}

bool AssetPack::extractTo(std::string_view name, const std::string &destinationPath) const {
    std::vector<std::uint8_t> bytes;
    if (!read(name, bytes)) return false;
    return writeWholeFile(destinationPath, bytes, impl_->error);
}

} // namespace emergent
