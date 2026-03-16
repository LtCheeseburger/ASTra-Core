// ASTra - APT XML helper
//
// We ship the legacy EA "aptconverter" code (AptFile) largely as-is.
// It was originally written assuming 32-bit pointer math and file-based IO.
//
// For the tool-suite we want an in-memory API, but for now we keep this
// wrapper extremely small and safe: write the provided buffers to a temporary
// directory, call AptFile::AptToXML() to generate an XML file next to the APT,
// then read the XML back into a string.
//
// This keeps gf_apt compiling cleanly on 64-bit while we continue modernizing
// the converter internals.

#include <gf/apt/apt_xml.hpp>

#include "aptconverter/Aptfile.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace gf::apt {

static bool write_file(const fs::path& p, const std::string& bytes, std::string* err) {
  std::ofstream f(p, std::ios::binary);
  if (!f) {
    if (err) *err = "Failed to open file for write: " + p.string();
    return false;
  }
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!f) {
    if (err) *err = "Failed to write file: " + p.string();
    return false;
  }
  return true;
}

static std::optional<std::string> read_file(const fs::path& p, std::string* err) {
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    if (err) *err = "Failed to open file for read: " + p.string();
    return std::nullopt;
  }
  std::string out;
  f.seekg(0, std::ios::end);
  const auto size = f.tellg();
  if (size < 0) {
    if (err) *err = "Failed to stat file: " + p.string();
    return std::nullopt;
  }
  out.resize(static_cast<size_t>(size));
  f.seekg(0, std::ios::beg);
  f.read(out.data(), static_cast<std::streamsize>(out.size()));
  if (!f) {
    if (err) *err = "Failed to read file: " + p.string();
    return std::nullopt;
  }
  return out;
}

static std::optional<std::string> apt_to_xml_string_from_buffers(const std::string& apt_bytes,
                                                                 const std::string& const_bytes,
                                                                 std::string* err) {
  try {
    const fs::path tmp = fs::temp_directory_path() / fs::path("astra_apt");
    fs::create_directories(tmp);

    // Use a unique-ish stem so parallel runs don't collide.
    const auto stem = std::string("apt_") + std::to_string(reinterpret_cast<uintptr_t>(&apt_bytes));
    const fs::path apt_path = tmp / (stem + ".apt");
    const fs::path const_path = tmp / (stem + ".const");
    const fs::path xml_path = tmp / (stem + ".xml");

    if (!write_file(apt_path, apt_bytes, err)) return std::nullopt;
    if (!write_file(const_path, const_bytes, err)) return std::nullopt;

    // AptFile expects a file path (and will look for the .const by stem).
    if (!AptFile::AptToXML(apt_path.string())) {
      if (err) *err = "AptFile::AptToXML returned failure";
      // Best-effort cleanup.
      std::error_code ec;
      fs::remove(apt_path, ec);
      fs::remove(const_path, ec);
      return std::nullopt;
    }

    auto xml = read_file(xml_path, err);

    // Best-effort cleanup.
    std::error_code ec;
    fs::remove(apt_path, ec);
    fs::remove(const_path, ec);
    fs::remove(xml_path, ec);

    return xml;
  } catch (const std::exception& e) {
    if (err) *err = std::string("Exception during APT->XML: ") + e.what();
    return std::nullopt;
  }
}

std::optional<std::string> apt_to_xml_string(const std::string& apt_path,
                                            const std::string& const_path,
                                            std::string* err) {
  // Read both inputs, then use the temp-file based converter.
  auto apt_bytes = read_file(fs::path(apt_path), err);
  if (!apt_bytes) return std::nullopt;
  auto const_bytes = read_file(fs::path(const_path), err);
  if (!const_bytes) return std::nullopt;
  return apt_to_xml_string_from_buffers(*apt_bytes, *const_bytes, err);
}

} // namespace gf::apt
