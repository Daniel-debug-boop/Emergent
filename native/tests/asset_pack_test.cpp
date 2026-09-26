// Asset pack self-test: writes real packs with Zstandard, reads them back, and
// attacks them.
//
// Round-tripping is the easy half. The half that matters is what happens to a
// pack that has been truncated, corrupted, or simply is not one: every one of
// those has to fail loudly, because the alternative is an engine that loads
// half a world and reports success.

#include "emergent/asset_pack.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
    if (condition) {
        std::printf("  ok    %s\n", what);
    } else {
        std::printf("  FAIL  %s\n", what);
        ++g_failures;
    }
}

std::string tempDir() {
    char pattern[] = "/tmp/emergent_pack_XXXXXX";
    const char *made = ::mkdtemp(pattern);
    return made ? std::string(made) : std::string();
}

bool writeBytes(const std::string &path, const std::vector<unsigned char> &bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!bytes.empty()) out.write(reinterpret_cast<const char *>(bytes.data()),
                                  static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

std::vector<unsigned char> readBytes(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

bool exists(const std::string &path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0;
}

// A payload with real redundancy, so compression is genuinely exercised rather
// than measured on data zstd cannot shrink.
std::string repetitive(std::size_t repeats) {
    std::string out;
    out.reserve(repeats * 64);
    for (std::size_t i = 0; i < repeats; ++i) {
        out += "EMERGENT vertex buffer: pos=3 normal=3 uv=2 material=1; ";
    }
    return out;
}

} // namespace

int main() {
    using emergent::AssetPack;
    using emergent::AssetWriter;

    std::printf("EMERGENT asset pack self-test (Zstandard)\n");

    const std::string dir = tempDir();
    check(!dir.empty(), "a scratch directory is created");
    if (dir.empty()) return 1;

    const std::string packPath = dir + "/world.ezpk";
    const std::string manifest = "{\"version\":1,\"seed\":173927}";
    const std::string bigText = repetitive(4096);
    const std::string binary(64, '\0'); // an entry of NUL bytes, not text

    // --- writing ----------------------------------------------------------
    {
        AssetWriter writer;
        check(!writer.isOpen(), "a fresh writer is not open");
        check(!writer.addText("manifest.json", manifest), "a closed writer refuses entries");
        check(!writer.error().empty(), "a refused entry records an error");
        check(writer.open(packPath, 9), "open() creates the pack");
        check(writer.isOpen(), "the writer reports open");
        check(!writer.open(packPath, 9), "an open writer refuses to be reopened");
        check(writer.entryCount() == 0, "a fresh pack holds no entries");

        check(writer.addText("manifest.json", manifest), "a text entry is added");
        check(writer.add("meshes/town.bin", binary.data(), binary.size()), "a binary entry is added");
        check(writer.addText("meshes/town.txt", bigText), "a large compressible entry is added");
        check(writer.add("meshes/empty.bin", nullptr, 0), "an empty entry is added");
        check(writer.entryCount() == 4, "every accepted entry is counted");
        check(writer.originalBytes() > 0, "original byte count accumulates");

        check(!writer.addText("manifest.json", "other"), "a duplicate name is refused");
        check(!writer.addText("", "x"), "an empty name is refused");
        check(!writer.add("bad", nullptr, 4), "a null buffer with a size is refused");
        check(writer.entryCount() == 4, "refused entries are not counted");

        check(writer.close(), "close() finalises the pack");
        check(!writer.isOpen(), "the writer is closed after close()");
        check(writer.close(), "close() is idempotent");
        check(exists(packPath), "the pack file exists on disk");
    }

    // --- reading ----------------------------------------------------------
    AssetPack pack;
    check(!pack.isOpen(), "a fresh pack is not open");
    check(!pack.contains("manifest.json"), "a closed pack contains nothing");
    check(pack.entrySize("manifest.json") == 0, "a closed pack has no entry sizes");

    check(pack.open(packPath), "the pack opens");
    check(pack.isOpen(), "the pack reports open");
    check(pack.entryCount() == 4, "every entry survives the round trip");
    check(pack.names().size() == 4, "names() lists every entry");

    for (const char *name : {"manifest.json", "meshes/town.bin", "meshes/town.txt", "meshes/empty.bin"}) {
        check(pack.contains(name), "contains() finds a written entry");
    }
    check(!pack.contains("missing"), "contains() rejects a name that was never written");
    check(!pack.contains(""), "contains() rejects an empty name");

    std::string readManifest;
    check(pack.readText("manifest.json", readManifest), "a text entry reads back");
    check(readManifest == manifest, "the text entry is byte-identical");

    std::vector<unsigned char> readBinary;
    check(pack.read("meshes/town.bin", readBinary), "a binary entry reads back");
    check(readBinary.size() == binary.size() && !readBinary.empty() &&
              std::memcmp(readBinary.data(), binary.data(), binary.size()) == 0,
        "the binary entry is byte-identical, NUL bytes included");

    std::vector<unsigned char> readBig;
    check(pack.read("meshes/town.txt", readBig), "a large entry reads back");
    check(readBig.size() == bigText.size(), "the large entry has the original size");
    check(readBig.size() == bigText.size() &&
              std::memcmp(readBig.data(), bigText.data(), bigText.size()) == 0,
        "the large entry is byte-identical");

    std::vector<unsigned char> readEmpty;
    check(pack.read("meshes/empty.bin", readEmpty), "an empty entry reads back");
    check(readEmpty.empty(), "an empty entry reads back empty");

    check(pack.entrySize("meshes/town.txt") == bigText.size(), "entrySize() matches the payload");
    check(pack.originalBytes() ==
              manifest.size() + binary.size() + bigText.size(),
        "originalBytes() sums the uncompressed entries");

    // The whole reason for shipping a pack: the same bytes, fewer of them.
    check(pack.storedBytes() < pack.originalBytes(), "the pack is smaller than its contents");
    check(pack.compressionRatio() > 0.0 && pack.compressionRatio() < 1.0,
        "compressionRatio() reports the real saving");
    std::printf("        %llu -> %llu bytes (%.1f%%)\n",
                static_cast<unsigned long long>(pack.originalBytes()),
                static_cast<unsigned long long>(pack.storedBytes()),
                pack.compressionRatio() * 100.0);

    // A second reader over the same file, to prove nothing is consumed on read.
    AssetPack second;
    check(second.open(packPath), "a second reader opens the same pack");
    check(second.entryCount() == pack.entryCount(), "both readers see every entry");

    // --- extraction -------------------------------------------------------
    const std::string extracted = dir + "/extracted.txt";
    check(pack.extractTo("meshes/town.txt", extracted), "an entry extracts to a file");
    check(exists(extracted), "the extracted file exists");
    {
        std::ifstream in(extracted, std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check(body == bigText, "the extracted file is byte-identical");
    }
    check(!pack.extractTo("missing", dir + "/nope.txt"), "extracting an unknown entry fails");
    check(!exists(dir + "/nope.txt"), "a failed extraction writes nothing");

    // --- attacks ----------------------------------------------------------
    const std::vector<unsigned char> original = readBytes(packPath);
    check(original.size() > 64, "the pack has a body to attack");

    {
        // Wrong magic.
        std::vector<unsigned char> broken = original;
        broken[0] = 'X';
        const std::string path = dir + "/badmagic.ezpk";
        writeBytes(path, broken);
        AssetPack hostile;
        check(!hostile.open(path), "a pack with a broken magic is rejected");
        check(!hostile.error().empty(), "a rejected pack explains itself");
        check(!hostile.isOpen() && hostile.entryCount() == 0, "a rejected pack exposes nothing");
    }
    {
        // Unsupported version.
        std::vector<unsigned char> broken = original;
        broken[8] = 99;
        const std::string path = dir + "/badversion.ezpk";
        writeBytes(path, broken);
        AssetPack hostile;
        check(!hostile.open(path), "an unsupported version is rejected");
    }
    {
        // Truncated: the index frame is gone entirely.
        const std::string path = dir + "/truncated.ezpk";
        writeBytes(path, std::vector<unsigned char>(original.begin(), original.end() - 32));
        AssetPack hostile;
        check(!hostile.open(path), "a pack truncated inside the index is rejected");
    }
    {
        // Header only.
        const std::string path = dir + "/headeronly.ezpk";
        writeBytes(path, std::vector<unsigned char>(original.begin(), original.begin() + 48));
        AssetPack hostile;
        check(!hostile.open(path), "a header with no body is rejected");
    }
    {
        // Too small to hold a header at all.
        const std::string path = dir + "/tiny.ezpk";
        writeBytes(path, std::vector<unsigned char>(original.begin(), original.begin() + 8));
        AssetPack hostile;
        check(!hostile.open(path), "a file smaller than a header is rejected");
    }
    {
        // Corrupt one byte inside a payload. A dedicated pack is used so the
        // damaged byte lands in a known place: the first entry's frame starts
        // immediately after the 48-byte header. The payload is random, so zstd
        // stores it as raw blocks and any flipped content byte necessarily
        // changes the decompressed output, which the frame checksum then
        // catches. (Flipping a byte in a highly compressible payload can land
        // in a field that decodes to the same value, which would make this
        // assertion unreliable rather than strict.)
        const std::string path = dir + "/corrupt.ezpk";
        {
            AssetWriter writer;
            writer.open(path, 3);
            std::vector<unsigned char> noisy(64 * 1024);
            std::random_device source;
            for (std::size_t i = 0; i < noisy.size(); ++i) {
                noisy[i] = static_cast<unsigned char>(source());
            }
            writer.add("a.bin", noisy.data(), noisy.size());
            writer.addText("b.txt", "still readable");
            writer.close();
        }
        std::vector<unsigned char> damaged = readBytes(path);
        check(damaged.size() > 64, "the corruptible pack has a body");
        damaged[64] = static_cast<unsigned char>(damaged[64] ^ 0x5au);
        writeBytes(path, damaged);

        AssetPack hostile;
        check(hostile.open(path), "a corrupt payload does not stop the index loading");
        check(hostile.entryCount() == 2, "both entries are still listed");
        std::vector<unsigned char> out;
        check(!hostile.read("a.bin", out), "reading a corrupt entry fails the checksum");
        std::string other;
        check(hostile.readText("b.txt", other), "an intact entry is still readable");
        check(other == "still readable", "the intact entry is byte-identical");
    }
    {
        // An index claiming an absurd entry count.
        std::vector<unsigned char> broken = original;
        broken[12] = 0xff;
        broken[13] = 0xff;
        const std::string path = dir + "/badcount.ezpk";
        writeBytes(path, broken);
        AssetPack hostile;
        check(!hostile.open(path), "a pack with an impossible entry count is rejected");
    }
    {
        AssetPack missing;
        check(!missing.open(dir + "/does-not-exist.ezpk"), "a missing file is reported, not crashed on");
        check(!missing.error().empty(), "a missing file explains itself");
    }

    // --- empty pack --------------------------------------------------------
    {
        const std::string path = dir + "/empty.ezpk";
        AssetWriter writer;
        check(writer.open(path, 3), "an empty pack opens for writing");
        check(writer.close(), "an empty pack finalises");
        check(exists(path), "the empty pack is written");
        AssetPack none;
        check(none.open(path), "an empty pack opens for reading");
        check(none.entryCount() == 0, "an empty pack has no entries");
        check(none.compressionRatio() == 0.0, "an empty pack has no ratio to report");
        std::vector<unsigned char> nothing;
        check(!none.read("anything", nothing), "reading from an empty pack fails");
    }

    // --- level validation ---------------------------------------------------
    {
        AssetWriter writer;
        check(!writer.open(dir + "/badlevel.ezpk", 0), "compression level 0 is rejected");
        check(!writer.open(dir + "/badlevel.ezpk", 99), "an out-of-range level is rejected");
        check(!writer.open(dir + "/badlevel.ezpk", -3), "a negative level is rejected");
    }

    pack.close();
    check(!pack.isOpen() && pack.entryCount() == 0, "close() empties the pack");

    if (g_failures == 0) {
        std::printf("\nall asset pack checks passed\n");
        return 0;
    }
    std::printf("\n%d asset pack check(s) FAILED\n", g_failures);
    return 1;
}
