#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gf/core/AstContainerEditor.hpp>

namespace gf::core {

struct AstAuthoringProfile {
  enum class SyntheticEntryKind {
    None,
    FakeRootCountOnly,
  };

  std::string donor_path;
  std::string donor_display_name;
  std::string game_family;
  std::string platform;
  std::string header_style;
  std::string endianness;
  AstContainerEditor::Header header{};
  std::vector<std::uint32_t> tags;

  bool requiresSyntheticEntry = false;
  SyntheticEntryKind synthetic_entry_kind = SyntheticEntryKind::None;
  std::int32_t header_file_count_bias = 0;
  std::int32_t directory_entry_count_bias = 0;
  bool uses_root_directory_stub = false;
  bool name_case_insensitive = true;
  char normalized_path_separator = '/';
  std::string donor_inspection_summary;

  static std::optional<AstAuthoringProfile> from_donor_ast(const std::filesystem::path& donor_path,
                                                           std::string* err = nullptr);
};

struct AstBuildEntry {
  enum class Type {
    Texture,
    Text,
    Raw,
  };

  enum class CompressionMode {
    None,
    Zlib,
  };

  std::filesystem::path source_path;
  std::string archive_name;
  Type detected_type = Type::Raw;
  std::string tag_category;
  CompressionMode compression = CompressionMode::None;
  std::vector<std::uint8_t> payload_bytes;
  bool include = true;
  bool valid = false;
  std::vector<std::string> warnings;
  std::vector<std::string> errors;
};

struct AstBuildValidationResult {
  bool ok = false;
  std::vector<std::string> warnings;
  std::vector<std::string> errors;
};

class AstBuildValidator final {
public:
  static AstBuildValidationResult validate(const AstAuthoringProfile& profile,
                                           const std::vector<AstBuildEntry>& entries);
};

class AstContainerBuilder final {
public:
  bool load_profile_from_donor(const std::filesystem::path& donor_path, std::string* err = nullptr);
  const std::optional<AstAuthoringProfile>& profile() const { return m_profile; }
  const std::vector<AstBuildEntry>& entries() const { return m_entries; }
  std::vector<AstBuildEntry>& mutable_entries() { return m_entries; }

  std::optional<AstBuildEntry> make_entry_from_file(const std::filesystem::path& source_path,
                                                    std::string archive_name = {},
                                                    AstBuildEntry::CompressionMode compression = AstBuildEntry::CompressionMode::None,
                                                    std::string* err = nullptr) const;
  std::size_t add_folder_entries(const std::filesystem::path& folder_path,
                                 std::string* err = nullptr,
                                 std::vector<std::string>* import_warnings = nullptr);
  bool add_entry(AstBuildEntry entry, std::string* err = nullptr);
  void remove_entry(std::size_t index);
  void clear_entries();

  AstBuildValidationResult validate() const;
  std::vector<std::uint8_t> build(std::string* err = nullptr) const;
  bool save_to_disk(const std::filesystem::path& output_path, std::string* err = nullptr) const;

private:
  std::optional<AstAuthoringProfile> m_profile;
  std::vector<AstBuildEntry> m_entries;
};

} // namespace gf::core
