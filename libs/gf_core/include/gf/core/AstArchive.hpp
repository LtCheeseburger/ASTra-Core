#pragma once

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <optional>
#include <istream>
#include <string>
#include <vector>

namespace gf::core {

// Lightweight AST/BGFA directory index reader.
//
// Intent:
//  - Fast, safe parsing for UI population (names/sizes/offsets only).
//  - No decompression, no data extraction yet.
//  - std-only (no Qt, no platform enums).
class AstArchive {
public:
  // When enabled, ASTra will emit trace logs describing embedded type detection
  // and naming decisions. Intended for debugging classification regressions.
  static void setDebugClassificationTrace(bool enabled);
  static bool debugClassificationTrace();

  struct Entry {
    std::uint32_t index = 0;
    std::uint64_t data_offset = 0;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint32_t flags = 0;
    std::uint64_t id_or_hash = 0; // best-effort; interpretation depends on flags
    std::string name;             // best-effort; may be empty
    // UI-friendly label. If present, this should be shown to users.
    // Keep `name` as a safe/base filename when possible.
    std::string display_name;
    std::string type_hint;        // best-effort from magic sniff (e.g., "DDS")
    std::string ext_hint;         // best-effort extension including dot (e.g., ".dds")
  };

  struct Index {
    std::uint32_t file_count = 0;
    std::uint32_t tag_count = 0;
    std::uint32_t file_name_length = 0;
    std::uint64_t dir_offset = 0;
    std::uint64_t dir_size = 0;

    bool is_ps3 = false;
    bool is_xbox = false;

    std::vector<Entry> entries;
  };

  // Returns an Index on success. On failure, returns nullopt and fills err (if provided).
  static std::optional<Index> readIndex(const std::filesystem::path& astPath,
                                        std::string* err = nullptr);

  // Reads an embedded AST (BGFA) index from within a parent AST file.
  // parentAstPath: path to the container .ast on disk
  // baseOffset: absolute file offset where embedded BGFA begins
  // maxReadable: safety cap (bytes) for parsing within parent file
    static std::optional<Index> readEmbeddedIndex(std::istream& stream,
                                          std::uint64_t baseOffset,
                                          std::uint64_t maxReadable,
                                          std::string* err = nullptr);

static std::optional<Index> readEmbeddedIndexFromFile(const std::filesystem::path& parentAstPath,
                                                        std::uint64_t baseOffset,
                                                        std::uint64_t maxReadable,
                                                        std::string* err = nullptr);
  // Like readEmbeddedIndexFromFile, but supports zlib-wrapped embedded ASTs.
  static std::optional<Index> readEmbeddedIndexFromFileSmart(const std::filesystem::path& parentAstPath,
                                                             std::uint64_t baseOffset,
                                                             std::uint64_t maxReadable,
                                                             std::string* err = nullptr);

  // Inflate up to `outCap` bytes from a zlib-compressed buffer.
  // Returns empty on failure.
  //
  // This is intended for UI preview (hex/text) and format sniffing only.
  static std::vector<std::uint8_t> inflateZlibPreview(const std::vector<std::uint8_t>& in,
                                                      std::size_t outCap = 4096);
};

} // namespace gf::core
