#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace gf::core {

// Minimal AST/BGFA writer used for in-place "replace entry" workflows.
//
// Scope (v0.6.x beta tooling):
//  - Preserve original header field sizes and directory schema.
//  - Allow replacing a single entry's stored bytes.
//  - Rebuild container safely (temp + backup) via gf::core::safe_write_bytes.
//
// Non-goals (for now):
//  - Adding/removing entries
//  - Renaming entries
//  - Deep understanding of EA flags (we preserve what we read)
class AstContainerEditor final {
public:
  enum class ReplaceMode {
    // Replace stored bytes exactly as provided.
    Raw,
    // If the original stored bytes look like a zlib stream, compress the replacement
    // and store it as a zlib stream. Otherwise behave like Raw.
    PreserveZlibIfPresent,
  };

  struct Header {
    std::uint32_t magicV = 0;
    std::uint32_t fakeFileCount = 0;
    std::uint32_t fileCount = 0;

    std::uint64_t dirOffset = 0;
    std::uint64_t dirSize = 0;

    std::uint8_t nametype = 0;
    std::uint8_t flagtype = 0;
    std::uint8_t tagsize = 0;
    std::uint8_t offsetsize = 0;
    std::uint8_t compsize = 0;
    std::uint8_t sizediffsize = 0;
    std::uint8_t shiftsize = 0;
    std::uint8_t unknownsize = 0;

    std::uint32_t tagCount = 0;
    std::uint32_t fileNameLength = 0;
  };

  struct Entry {
    std::uint32_t index = 0;
    std::uint32_t flags = 0;                 // combined record flags (recordFlagSize + flagtype)
    std::vector<std::uint8_t> tagBytes;      // tagsize bytes
    std::uint64_t dataOffset = 0;            // absolute offset in file
    std::uint64_t compressedSize = 0;
    std::uint64_t uncompressedSize = 0;
    std::uint64_t unknownField = 0;          // unknownsize bytes (preserved)
    std::vector<std::uint8_t> nameBytes;     // fileNameLength bytes

    std::vector<std::uint8_t> storedBytes;   // bytes as stored (raw or compressed)
  };

  static std::optional<AstContainerEditor> load(const std::filesystem::path& astPath,
                                                std::string* err = nullptr);

  const Header& header() const { return m_header; }
  const std::filesystem::path& path() const { return m_path; }
  const std::vector<std::uint32_t>& tags() const { return m_tags; }
  const std::vector<Entry>& entries() const { return m_entries; }

  

// Returns the entry's bytes exactly as stored in the container (raw or compressed).
// Returns nullopt if entryIndex is invalid.
std::optional<std::vector<std::uint8_t>> getEntryStoredBytes(std::uint32_t entryIndex) const;

// Returns the entry's bytes inflated if the stored payload looks like a zlib stream.
// If the payload is not zlib, returns the stored bytes.
// Returns nullopt on invalid entryIndex or inflate failure (err filled if provided).
std::optional<std::vector<std::uint8_t>> getEntryInflatedBytes(std::uint32_t entryIndex,
                                                               std::string* err = nullptr) const;

bool replaceEntryBytes(std::uint32_t entryIndex,
                         std::span<const std::uint8_t> newBytes,
                         ReplaceMode mode,
                         std::string* err = nullptr);

  bool writeInPlace(std::string* err = nullptr,
                    bool makeBackup = true,
                    std::uint64_t maxWriteBytes = 0);

  bool rebuildToStream(std::ostream& os, std::string* err = nullptr) const;
  std::vector<std::uint8_t> rebuild(std::string* err = nullptr) const;

  // Validates the structural integrity of raw BGFA bytes (e.g. the output of
  // rebuild()).  Returns true on success; fills *err with an actionable message
  // on the first detected problem.  Does NOT load or decompress entry payloads.
  static bool validate(std::span<const std::uint8_t> data, std::string* err = nullptr);

  // Structured validation result — preferred for callers that need to log or
  // surface issues rather than just pass/fail.
  struct AstValidationReport {
    bool ok = false;
    std::string firstError;           // empty when ok == true
    std::vector<std::string> warnings; // non-fatal issues (currently reserved)
  };

  // Wraps validate() and returns a structured report.  Equivalent semantics:
  // ok == false means the data should NOT be written to disk as-is.
  static AstValidationReport validateWithReport(std::span<const std::uint8_t> data);

private:
  std::filesystem::path m_path;
  Header m_header{};
  std::vector<std::uint32_t> m_tags;
  std::vector<Entry> m_entries;
};

} // namespace gf::core
