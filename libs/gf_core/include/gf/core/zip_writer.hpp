#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gf::core {

// Minimal ZIP writer (store/no-compression) for small diagnostic bundles.
// Uses standard ZIP file format with method=0 (stored).
class ZipWriter final {
public:
  struct Entry {
    std::string name;
    std::vector<std::uint8_t> data;
  };

  ZipWriter() = default;

  // Add a file entry. `name` must use forward slashes (e.g. "diagnostics/scan_result.json").
  void add_file(std::string_view name, std::vector<std::uint8_t> data);

  // Convenience for text.
  void add_text(std::string_view name, std::string_view text);

  // Write the final zip file to `zipPath` (overwrites if exists).
  // Returns true on success.
  bool write_to_file(const std::string& zipPath, std::string* outErr) const;

private:
  std::vector<Entry> m_entries;
};

} // namespace gf::core
