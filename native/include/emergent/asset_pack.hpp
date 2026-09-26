#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace emergent {

// Content delivery for EMERGENT's shipped assets, built on Zstandard.
//
// A pack is a single file holding many named blobs, each compressed
// independently so any one of them can be pulled out without touching the rest.
// Independent frames also mean a corrupt entry damages only that entry.
//
// Layout, all integers little-endian:
//
//   [ 48-byte header ]
//   [ payload frame 0 ] [ payload frame 1 ] ...   zstd frames, back to back
//   [ index frame ]                             one zstd frame
//
// The index frame decompresses to the entry table followed by the name blob:
// an array of (name offset, name length, data offset, compressed size,
// original size) records, then every entry name concatenated.
//
// Integrity is not a checksum EMERGENT invented. Every frame, payload and
// index alike, is a zstd frame, and zstd verifies its own content checksum
// during decompression. A truncated, reordered or corrupted pack therefore
// fails to load or fails to read the affected entry, rather than quietly
// yielding garbage geometry or an empty save file.

// Streaming writer. Entries are appended as they are added; the index is
// written by close(), which is the only call that produces a loadable file.
class AssetWriter {
public:
    AssetWriter();
    ~AssetWriter();
    AssetWriter(AssetWriter&&) noexcept;
    AssetWriter& operator=(AssetWriter&&) noexcept;
    AssetWriter(const AssetWriter&) = delete;
    AssetWriter& operator=(const AssetWriter&) = delete;

    // Creates or truncates `path`. `level` is a Zstandard compression level;
    // values outside [1, 19] are rejected rather than clamped, so a typo in a
    // build script fails loudly.
    bool open(const std::string &path, int level = 9);
    bool isOpen() const noexcept;
    const std::string &path() const noexcept;

    // Adds one entry. An empty name, a duplicate name or a null buffer with a
    // non-zero size is rejected; the writer stays open so the caller can
    // correct the input and carry on.
    bool add(std::string_view name, const void *data, std::size_t size);
    bool addText(std::string_view name, std::string_view text);
    // Reads `sourcePath` and stores it under `name`.
    bool addFile(std::string_view name, const std::string &sourcePath);
    // Recursively adds every regular file under `sourceDirectory`, storing
    // each as "<prefix>/<path relative to sourceDirectory>".
    bool addDirectory(std::string_view prefix, const std::string &sourceDirectory);

    std::size_t entryCount() const noexcept;
    std::uint64_t originalBytes() const noexcept;
    std::uint64_t storedBytes() const noexcept;
    // Empty until the first failure. Never cleared by a later success, so a
    // caller that checks once at the end still sees the first problem.
    const std::string &error() const noexcept;

    // Writes the index and closes the file. Returns false if the file cannot
    // be finalised, in which case the output is not a valid pack and the
    // error string says why. Idempotent.
    bool close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Read side. The whole pack is held in memory: packs are content bundles of a
// few megabytes, and a single mapping keeps entry lookup and reads free of
// per-call file I/O.
class AssetPack {
public:
    AssetPack();
    ~AssetPack();
    AssetPack(AssetPack&&) noexcept;
    AssetPack& operator=(AssetPack&&) noexcept;
    AssetPack(const AssetPack&) = delete;
    AssetPack& operator=(const AssetPack&) = delete;

    bool open(const std::string &path);
    void close();
    bool isOpen() const noexcept;
    const std::string &path() const noexcept;

    std::size_t entryCount() const noexcept;
    bool contains(std::string_view name) const;
    std::vector<std::string> names() const;
    // Uncompressed size of an entry, or 0 if it is not in the pack.
    std::size_t entrySize(std::string_view name) const;

    // Reads and decompresses one entry. Returns false for an unknown name, a
    // failed checksum or a size that does not match the index. `out` is
    // replaced only on success.
    bool read(std::string_view name, std::vector<std::uint8_t> &out) const;
    bool readText(std::string_view name, std::string &out) const;
    // Writes one entry to `destinationPath`.
    bool extractTo(std::string_view name, const std::string &destinationPath) const;

    std::uint64_t originalBytes() const noexcept;
    std::uint64_t storedBytes() const noexcept;
    // storedBytes / originalBytes, or 0 for an empty pack. A pack of
    // incompressible data legitimately reports below 1.
    double compressionRatio() const noexcept;
    const std::string &error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace emergent
