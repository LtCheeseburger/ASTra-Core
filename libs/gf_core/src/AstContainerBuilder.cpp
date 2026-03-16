#include <gf/core/AstContainerBuilder.hpp>

#include <gf/core/safe_write.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_set>

#include <zlib.h>

namespace gf::core {
namespace {

static std::string trim_zero_terminated(const std::vector<std::uint8_t>& name_bytes) {
  std::size_t n = 0;
  while (n < name_bytes.size() && name_bytes[n] != 0) ++n;
  return std::string(reinterpret_cast<const char*>(name_bytes.data()), n);
}

static std::string lower_ascii(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

static std::string detect_type_name(std::span<const std::uint8_t> bytes, const std::filesystem::path& path) {
  const auto ext = lower_ascii(path.extension().string());
  if (ext == ".dds" || ext == ".xpr" || ext == ".xpr2" || ext == ".p3r") return "texture";
  if (ext == ".xml" || ext == ".txt" || ext == ".cfg" || ext == ".ini" || ext == ".json" || ext == ".rsf") return "text";
  if (bytes.size() >= 5 && bytes[0] == '<' && bytes[1] == '?' && bytes[2] == 'x' && bytes[3] == 'm' && bytes[4] == 'l') return "text";
  if (!bytes.empty() && (bytes[0] == '<' || bytes[0] == '{' || bytes[0] == '[')) return "text";
  if (bytes.size() >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ') return "texture";
  return "raw";
}

static std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (err) *err = "Failed to open source file.";
    return {};
  }
  f.seekg(0, std::ios::end);
  const auto size = static_cast<std::uint64_t>(f.tellg());
  f.seekg(0, std::ios::beg);
  if (size > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
    if (err) *err = "Source file is too large.";
    return {};
  }
  std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
  if (!out.empty() && !f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()))) {
    if (err) *err = "Failed to read source file bytes.";
    return {};
  }
  return out;
}

static std::vector<std::uint8_t> zlib_deflate(std::span<const std::uint8_t> in, std::string* err) {
  z_stream zs{};
  int rc = ::deflateInit(&zs, Z_BEST_COMPRESSION);
  if (rc != Z_OK) {
    if (err) *err = "deflateInit failed";
    return {};
  }

  zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
  zs.avail_in = static_cast<uInt>(std::min<std::size_t>(in.size(), static_cast<std::size_t>(std::numeric_limits<uInt>::max())));

  std::vector<std::uint8_t> out;
  std::array<std::uint8_t, 64 * 1024> tmp{};
  while (true) {
    zs.next_out = reinterpret_cast<Bytef*>(tmp.data());
    zs.avail_out = static_cast<uInt>(tmp.size());
    rc = ::deflate(&zs, zs.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH);
    const std::size_t produced = tmp.size() - static_cast<std::size_t>(zs.avail_out);
    if (produced) out.insert(out.end(), tmp.data(), tmp.data() + produced);
    if (rc == Z_STREAM_END) break;
    if (rc != Z_OK) {
      ::deflateEnd(&zs);
      if (err) *err = "deflate failed";
      return {};
    }
  }
  ::deflateEnd(&zs);
  return out;
}

static bool write_le(std::ostream& os, std::uint64_t v, std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i) {
    const char b = static_cast<char>((v >> (8ull * i)) & 0xFFu);
    if (!os.write(&b, 1)) return false;
  }
  return true;
}

template <typename T>
static bool write_le(std::ostream& os, T v) {
  return write_le(os, static_cast<std::uint64_t>(v), sizeof(T));
}

static bool write_directory_entry(std::ostream& os,
                                  const AstContainerEditor::Entry& e,
                                  const AstContainerEditor::Header& h,
                                  std::uint64_t new_offset) {
  const std::uint8_t flags_width = static_cast<std::uint8_t>(1u + h.flagtype);
  if (!write_le(os, static_cast<std::uint64_t>(e.flags), flags_width)) return false;

  std::vector<std::uint8_t> tag_bytes = e.tagBytes;
  tag_bytes.resize(h.tagsize, 0);
  if (!tag_bytes.empty() && !os.write(reinterpret_cast<const char*>(tag_bytes.data()), static_cast<std::streamsize>(tag_bytes.size()))) return false;

  const std::uint64_t base = (h.shiftsize >= 64) ? 0ull : (new_offset >> h.shiftsize);
  if (!write_le(os, base, h.offsetsize)) return false;
  if (!write_le(os, e.compressedSize, h.compsize)) return false;

  if (h.sizediffsize) {
    const std::uint64_t size_diff = (e.uncompressedSize > e.compressedSize) ? (e.uncompressedSize - e.compressedSize) : 0ull;
    if (!write_le(os, size_diff, h.sizediffsize)) return false;
  }
  if (h.unknownsize && !write_le(os, e.unknownField, h.unknownsize)) return false;

  std::vector<std::uint8_t> name_bytes = e.nameBytes;
  name_bytes.resize(h.fileNameLength, 0);
  if (!name_bytes.empty() && !os.write(reinterpret_cast<const char*>(name_bytes.data()), static_cast<std::streamsize>(name_bytes.size()))) return false;
  return static_cast<bool>(os);
}

static bool align_to(std::ostream& os, std::uint64_t alignment, std::uint64_t cur) {
  if (alignment <= 1) return true;
  const std::uint64_t pad = (alignment - (cur % alignment)) % alignment;
  std::array<char, 4096> zeros{};
  std::uint64_t remaining = pad;
  while (remaining > 0) {
    const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, zeros.size()));
    if (!os.write(zeros.data(), static_cast<std::streamsize>(chunk))) return false;
    remaining -= chunk;
  }
  return true;
}

static std::vector<std::uint8_t> build_name_bytes(const std::string& archive_name, std::uint32_t field_len) {
  std::vector<std::uint8_t> out(field_len, 0);
  const auto count = std::min<std::size_t>(archive_name.size(), out.size());
  if (count) std::memcpy(out.data(), archive_name.data(), count);
  return out;
}

static std::string normalize_archive_name(const std::filesystem::path& path, const AstAuthoringProfile& profile) {
  std::string s = path.generic_string();
  while (!s.empty() && (s.front() == '/' || s.front() == '\\')) s.erase(s.begin());
  if (profile.normalized_path_separator != '/') {
    for (char& ch : s) if (ch == '/') ch = profile.normalized_path_separator;
  }
  return s;
}

static std::string normalize_archive_name(const std::string& name, const AstAuthoringProfile& profile) {
  return normalize_archive_name(std::filesystem::path(name), profile);
}

static bool should_skip_import_file(const std::filesystem::path& path) {
  const auto name = lower_ascii(path.filename().string());
  if (name.empty()) return true;
  if (name == ".ds_store" || name == "thumbs.db" || name == "desktop.ini" || name == ".gitkeep") return true;
  if (name.size() >= 2 && name[0] == '.' && name[1] != '.') return true;
  return false;
}

static std::string profile_summary_text(const AstAuthoringProfile& profile,
                                        std::size_t parsed_entries,
                                        bool donor_requires_fake_bias) {
  std::ostringstream ss;
  ss << "donor=" << profile.donor_display_name
     << "; headerStyle=" << profile.header_style
     << "; endian=" << profile.endianness
     << "; parsedEntries=" << parsed_entries
     << "; tagCount=" << profile.header.tagCount
     << "; fileNameLength=" << profile.header.fileNameLength
     << "; shiftsize=" << unsigned(profile.header.shiftsize)
     << "; flagsWidth=" << unsigned(1u + profile.header.flagtype)
     << "; dirOffset=" << profile.header.dirOffset
     << "; donorHeaderFileCount=" << profile.header.fileCount
     << "; donorHeaderFakeFileCount=" << profile.header.fakeFileCount
     << "; requiresSyntheticEntry=" << (profile.requiresSyntheticEntry ? "yes" : "no")
     << "; headerFileCountBias=" << profile.header_file_count_bias
     << "; directoryEntryCountBias=" << profile.directory_entry_count_bias
     << "; fakeBiasDetected=" << (donor_requires_fake_bias ? "yes" : "no");
  return ss.str();
}

} // namespace

std::optional<AstAuthoringProfile> AstAuthoringProfile::from_donor_ast(const std::filesystem::path& donor_path,
                                                                       std::string* err) {
  if (err) err->clear();
  auto ed = AstContainerEditor::load(donor_path, err);
  if (!ed) return std::nullopt;

  AstAuthoringProfile profile;
  profile.donor_path = donor_path.string();
  profile.donor_display_name = donor_path.filename().string();
  profile.header = ed->header();
  profile.tags = ed->tags();
  profile.header_style = "BGFA";
  profile.endianness = "little";
  profile.platform = "derived-from-donor";
  profile.game_family = "derived-from-donor";

  const bool fake_bias = (profile.header.fakeFileCount == profile.header.fileCount + 1u);
  profile.requiresSyntheticEntry = fake_bias;
  profile.synthetic_entry_kind = fake_bias ? AstAuthoringProfile::SyntheticEntryKind::FakeRootCountOnly : AstAuthoringProfile::SyntheticEntryKind::None;
  profile.header_file_count_bias = 0;
  profile.directory_entry_count_bias = 0;
  profile.uses_root_directory_stub = false;
  profile.name_case_insensitive = true;
  profile.normalized_path_separator = '/';
  profile.donor_inspection_summary = profile_summary_text(profile, ed->entries().size(), fake_bias);
  return profile;
}

AstBuildValidationResult AstBuildValidator::validate(const AstAuthoringProfile& profile,
                                                     const std::vector<AstBuildEntry>& entries) {
  AstBuildValidationResult out;
  std::unordered_set<std::string> names;
  const std::size_t name_cap = static_cast<std::size_t>(profile.header.fileNameLength);
  std::size_t included_count = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];
    if (!entry.include) continue;
    ++included_count;
    if (entry.archive_name.empty()) out.errors.push_back("Entry " + std::to_string(i) + " has no archive name.");
    if (entry.payload_bytes.empty() && std::filesystem::file_size(entry.source_path) != 0) out.errors.push_back("Entry " + std::to_string(i) + " has no payload bytes.");
    if (entry.archive_name.size() > name_cap && name_cap > 0) {
      out.errors.push_back("Entry '" + entry.archive_name + "' exceeds donor name field length of " + std::to_string(name_cap) + " bytes.");
    }
    const auto normalized_name = profile.name_case_insensitive ? lower_ascii(entry.archive_name) : entry.archive_name;
    if (!names.insert(normalized_name).second) out.errors.push_back("Duplicate archive name collision: " + entry.archive_name);
    for (const auto& err : entry.errors) out.errors.push_back(err);
    for (const auto& warn : entry.warnings) out.warnings.push_back(warn);
  }
  if (included_count == 0) out.errors.push_back("Add at least one file before building the AST.");
  out.ok = out.errors.empty();
  return out;
}

bool AstContainerBuilder::load_profile_from_donor(const std::filesystem::path& donor_path, std::string* err) {
  m_profile = AstAuthoringProfile::from_donor_ast(donor_path, err);
  return m_profile.has_value();
}

std::optional<AstBuildEntry> AstContainerBuilder::make_entry_from_file(const std::filesystem::path& source_path,
                                                                       std::string archive_name,
                                                                       AstBuildEntry::CompressionMode compression,
                                                                       std::string* err) const {
  if (err) err->clear();
  if (!m_profile.has_value()) {
    if (err) *err = "No donor/template profile is loaded.";
    return std::nullopt;
  }
  AstBuildEntry entry;
  entry.source_path = source_path;
  entry.archive_name = archive_name.empty() ? normalize_archive_name(source_path.filename(), *m_profile)
                                            : normalize_archive_name(archive_name, *m_profile);
  entry.compression = compression;
  entry.payload_bytes = read_file_bytes(source_path, err);
  if (entry.payload_bytes.empty() && std::filesystem::exists(source_path) && std::filesystem::file_size(source_path) != 0) return std::nullopt;

  const auto type_name = detect_type_name(std::span<const std::uint8_t>(entry.payload_bytes.data(), entry.payload_bytes.size()), source_path);
  if (type_name == "texture") entry.detected_type = AstBuildEntry::Type::Texture;
  else if (type_name == "text") entry.detected_type = AstBuildEntry::Type::Text;
  else entry.detected_type = AstBuildEntry::Type::Raw;

  entry.tag_category = type_name;
  entry.valid = true;
  return entry;
}

std::size_t AstContainerBuilder::add_folder_entries(const std::filesystem::path& folder_path,
                                                    std::string* err,
                                                    std::vector<std::string>* import_warnings) {
  if (err) err->clear();
  if (import_warnings) import_warnings->clear();
  if (!m_profile.has_value()) {
    if (err) *err = "No donor/template profile is loaded.";
    return 0;
  }
  std::error_code ec;
  if (!std::filesystem::exists(folder_path, ec) || !std::filesystem::is_directory(folder_path, ec)) {
    if (err) *err = "Selected folder does not exist or is not a directory.";
    return 0;
  }

  std::vector<std::filesystem::path> files;
  for (std::filesystem::recursive_directory_iterator it(folder_path, ec), end; !ec && it != end; it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    const auto p = it->path();
    if (should_skip_import_file(p)) {
      if (import_warnings) import_warnings->push_back("Skipped junk/system file: " + p.string());
      continue;
    }
    files.push_back(p);
  }
  if (ec) {
    if (err) *err = "Failed while scanning selected folder.";
    return 0;
  }

  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
    return a.generic_string() < b.generic_string();
  });

  std::size_t added = 0;
  for (const auto& p : files) {
    std::string local_err;
    const auto rel = std::filesystem::relative(p, folder_path, ec);
    const auto archive_name = normalize_archive_name(ec ? p.filename() : rel, *m_profile);
    auto entry = make_entry_from_file(p, archive_name, AstBuildEntry::CompressionMode::None, &local_err);
    if (!entry.has_value()) {
      if (import_warnings) import_warnings->push_back("Failed to import " + p.string() + ": " + local_err);
      continue;
    }
    if (!add_entry(std::move(*entry), &local_err)) {
      if (import_warnings) import_warnings->push_back("Failed to add " + p.string() + ": " + local_err);
      continue;
    }
    ++added;
  }
  return added;
}

bool AstContainerBuilder::add_entry(AstBuildEntry entry, std::string* err) {
  if (err) err->clear();
  if (!m_profile.has_value()) {
    if (err) *err = "No donor/template profile is loaded.";
    return false;
  }
  if (!entry.valid) {
    if (err) *err = "Entry is not in a valid import state.";
    return false;
  }
  m_entries.push_back(std::move(entry));
  return true;
}

void AstContainerBuilder::remove_entry(std::size_t index) {
  if (index >= m_entries.size()) return;
  m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
}

void AstContainerBuilder::clear_entries() { m_entries.clear(); }

AstBuildValidationResult AstContainerBuilder::validate() const {
  if (!m_profile.has_value()) {
    AstBuildValidationResult out;
    out.errors.push_back("No donor/template profile is loaded.");
    return out;
  }
  return AstBuildValidator::validate(*m_profile, m_entries);
}

std::vector<std::uint8_t> AstContainerBuilder::build(std::string* err) const {
  if (err) err->clear();
  if (!m_profile.has_value()) {
    if (err) *err = "No donor/template profile is loaded.";
    return {};
  }

  const auto validation = validate();
  if (!validation.ok) {
    if (err) {
      std::ostringstream ss;
      for (std::size_t i = 0; i < validation.errors.size(); ++i) {
        if (i) ss << "\n";
        ss << validation.errors[i];
      }
      *err = ss.str();
    }
    return {};
  }

  auto header = m_profile->header;
  std::vector<AstContainerEditor::Entry> built_entries;
  built_entries.reserve(m_entries.size());

  const std::vector<std::uint8_t> default_tag_bytes = [&]() {
    if (header.tagsize == 0) return std::vector<std::uint8_t>{};
    return std::vector<std::uint8_t>(header.tagsize, 0);
  }();

  std::uint64_t original_offset_hint = 0;
  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    const auto& src = m_entries[i];
    if (!src.include) continue;
    AstContainerEditor::Entry dst;
    dst.index = static_cast<std::uint32_t>(built_entries.size());
    dst.flags = 0;
    dst.tagBytes = default_tag_bytes;
    dst.dataOffset = original_offset_hint;
    dst.unknownField = 0;
    dst.nameBytes = build_name_bytes(src.archive_name, header.fileNameLength);
    dst.uncompressedSize = static_cast<std::uint64_t>(src.payload_bytes.size());

    if (src.compression == AstBuildEntry::CompressionMode::Zlib) {
      dst.storedBytes = zlib_deflate(std::span<const std::uint8_t>(src.payload_bytes.data(), src.payload_bytes.size()), err);
      if (dst.storedBytes.empty() && !src.payload_bytes.empty()) {
        if (err && err->empty()) *err = "Failed to compress build entry.";
        return {};
      }
    } else {
      dst.storedBytes = src.payload_bytes;
    }
    dst.compressedSize = static_cast<std::uint64_t>(dst.storedBytes.size());
    original_offset_hint += dst.compressedSize + 1;
    built_entries.push_back(std::move(dst));
  }

  header.fileCount = static_cast<std::uint32_t>(static_cast<std::int64_t>(built_entries.size()) + m_profile->header_file_count_bias);
  if (m_profile->requiresSyntheticEntry && m_profile->synthetic_entry_kind == AstAuthoringProfile::SyntheticEntryKind::FakeRootCountOnly) {
    header.fakeFileCount = static_cast<std::uint32_t>(static_cast<std::int64_t>(header.fileCount) + 1);
  } else {
    header.fakeFileCount = header.fileCount;
  }

  const std::uint8_t flags_width = static_cast<std::uint8_t>(1u + header.flagtype);
  const std::uint64_t record_size = static_cast<std::uint64_t>(flags_width)
      + static_cast<std::uint64_t>(header.tagsize)
      + static_cast<std::uint64_t>(header.offsetsize)
      + static_cast<std::uint64_t>(header.compsize)
      + static_cast<std::uint64_t>(header.sizediffsize)
      + static_cast<std::uint64_t>(header.unknownsize)
      + static_cast<std::uint64_t>(header.fileNameLength);
  header.dirOffset = 64;
  header.dirSize = static_cast<std::uint64_t>(header.tagCount) * 4ull
                 + record_size * static_cast<std::uint64_t>(static_cast<std::int64_t>(built_entries.size()) + m_profile->directory_entry_count_bias);

  const std::uint64_t alignment = (header.shiftsize >= 63) ? 1ull : (1ull << header.shiftsize);
  std::vector<const AstContainerEditor::Entry*> sorted_by_offset;
  for (const auto& e : built_entries) sorted_by_offset.push_back(&e);
  std::stable_sort(sorted_by_offset.begin(), sorted_by_offset.end(), [](const auto* a, const auto* b) { return a->dataOffset < b->dataOffset; });

  std::vector<std::uint64_t> new_offsets(built_entries.size(), 0);
  std::uint64_t cursor = header.dirOffset + header.dirSize;
  for (const auto* ep : sorted_by_offset) {
    if (alignment > 1) cursor += (alignment - (cursor % alignment)) % alignment;
    new_offsets[ep->index] = cursor;
    cursor += ep->storedBytes.size();
  }

  std::ostringstream os(std::ios::out | std::ios::binary);
  if (!os.write("BGFA", 4) ||
      !write_le(os, header.magicV) ||
      !write_le(os, header.fakeFileCount) ||
      !write_le(os, header.fileCount) ||
      !write_le(os, header.dirOffset, 8) ||
      !write_le(os, header.dirSize, 8) ||
      !write_le(os, header.nametype) ||
      !write_le(os, header.flagtype) ||
      !write_le(os, header.tagsize) ||
      !write_le(os, header.offsetsize) ||
      !write_le(os, header.compsize) ||
      !write_le(os, header.sizediffsize) ||
      !write_le(os, header.shiftsize) ||
      !write_le(os, header.unknownsize) ||
      !write_le(os, header.tagCount) ||
      !write_le(os, header.fileNameLength)) {
    if (err) *err = "Failed to write AST header.";
    return {};
  }
  std::array<char, 16> reserved{};
  os.write(reserved.data(), static_cast<std::streamsize>(reserved.size()));
  for (auto tag : m_profile->tags) {
    if (!write_le(os, tag)) {
      if (err) *err = "Failed to write AST tags.";
      return {};
    }
  }
  for (const auto& e : built_entries) {
    if (!write_directory_entry(os, e, header, new_offsets[e.index])) {
      if (err) *err = "Failed to write AST directory entry.";
      return {};
    }
  }
  for (const auto* ep : sorted_by_offset) {
    if (!align_to(os, alignment, new_offsets[ep->index])) {
      if (err) *err = "Failed to write AST alignment padding.";
      return {};
    }
    if (!ep->storedBytes.empty()) os.write(reinterpret_cast<const char*>(ep->storedBytes.data()), static_cast<std::streamsize>(ep->storedBytes.size()));
  }
  const auto str = os.str();
  std::vector<std::uint8_t> out(str.begin(), str.end());
  if (!AstContainerEditor::validate(std::span<const std::uint8_t>(out.data(), out.size()), err)) return {};
  return out;
}

bool AstContainerBuilder::save_to_disk(const std::filesystem::path& output_path, std::string* err) const {
  const auto bytes = build(err);
  if (bytes.empty()) return false;
  SafeWriteOptions opt;
  opt.make_backup = false;
  const auto rc = safe_write_bytes(output_path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()), opt);
  if (!rc.ok) {
    if (err) *err = rc.message;
    return false;
  }
  auto reopened = AstContainerEditor::load(output_path, err);
  if (!reopened.has_value()) {
    if (err && err->empty()) *err = "Generated AST could not be reopened after build.";
    return false;
  }
  return true;
}

} // namespace gf::core
