#include "gf/core/zip_writer.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <zlib.h>

namespace gf::core {

namespace {

static void write_u16(std::ofstream& os, std::uint16_t v) {
  os.put(static_cast<char>(v & 0xFF));
  os.put(static_cast<char>((v >> 8) & 0xFF));
}

static void write_u32(std::ofstream& os, std::uint32_t v) {
  os.put(static_cast<char>(v & 0xFF));
  os.put(static_cast<char>((v >> 8) & 0xFF));
  os.put(static_cast<char>((v >> 16) & 0xFF));
  os.put(static_cast<char>((v >> 24) & 0xFF));
}

struct CentralRecord {
  std::string name;
  std::uint32_t crc32 = 0;
  std::uint32_t compSize = 0;
  std::uint32_t uncompSize = 0;
  std::uint32_t localHeaderOffset = 0;
};

} // namespace

void ZipWriter::add_file(std::string_view name, std::vector<std::uint8_t> data) {
  Entry e;
  e.name.assign(name.begin(), name.end());
  e.data = std::move(data);
  m_entries.push_back(std::move(e));
}

void ZipWriter::add_text(std::string_view name, std::string_view text) {
  std::vector<std::uint8_t> data(text.begin(), text.end());
  add_file(name, std::move(data));
}

bool ZipWriter::write_to_file(const std::string& zipPath, std::string* outErr) const {
  try {
    std::filesystem::path p(zipPath);
    if (p.has_parent_path()) {
      std::filesystem::create_directories(p.parent_path());
    }

    std::ofstream os(zipPath, std::ios::binary | std::ios::trunc);
    if (!os.is_open()) {
      if (outErr) *outErr = "Failed to open zip for writing.";
      return false;
    }

    std::vector<CentralRecord> centrals;
    centrals.reserve(m_entries.size());

    for (const auto& e : m_entries) {
      const std::uint32_t localOffset = static_cast<std::uint32_t>(os.tellp());

      // Compute CRC32
      uLong crc = ::crc32(0L, Z_NULL, 0);
      if (!e.data.empty()) {
        crc = ::crc32(crc, reinterpret_cast<const Bytef*>(e.data.data()),
                      static_cast<uInt>(e.data.size()));
      }

      const std::uint32_t size = static_cast<std::uint32_t>(e.data.size());

      // Local file header
      write_u32(os, 0x04034b50); // signature
      write_u16(os, 20);         // version needed
      write_u16(os, 0);          // flags
      write_u16(os, 0);          // compression method (0=stored)
      write_u16(os, 0);          // mod time
      write_u16(os, 0);          // mod date
      write_u32(os, static_cast<std::uint32_t>(crc));
      write_u32(os, size);       // compressed size
      write_u32(os, size);       // uncompressed size
      write_u16(os, static_cast<std::uint16_t>(e.name.size()));
      write_u16(os, 0);          // extra length

      os.write(e.name.data(), static_cast<std::streamsize>(e.name.size()));
      if (!e.data.empty()) {
        os.write(reinterpret_cast<const char*>(e.data.data()),
                 static_cast<std::streamsize>(e.data.size()));
      }

      CentralRecord c;
      c.name = e.name;
      c.crc32 = static_cast<std::uint32_t>(crc);
      c.compSize = size;
      c.uncompSize = size;
      c.localHeaderOffset = localOffset;
      centrals.push_back(std::move(c));
    }

    const std::uint32_t centralDirOffset = static_cast<std::uint32_t>(os.tellp());

    // Central directory
    for (const auto& c : centrals) {
      write_u32(os, 0x02014b50); // signature
      write_u16(os, 20);         // version made by
      write_u16(os, 20);         // version needed
      write_u16(os, 0);          // flags
      write_u16(os, 0);          // method
      write_u16(os, 0);          // mod time
      write_u16(os, 0);          // mod date
      write_u32(os, c.crc32);
      write_u32(os, c.compSize);
      write_u32(os, c.uncompSize);
      write_u16(os, static_cast<std::uint16_t>(c.name.size()));
      write_u16(os, 0);          // extra len
      write_u16(os, 0);          // comment len
      write_u16(os, 0);          // disk start
      write_u16(os, 0);          // internal attrs
      write_u32(os, 0);          // external attrs
      write_u32(os, c.localHeaderOffset);
      os.write(c.name.data(), static_cast<std::streamsize>(c.name.size()));
    }

    const std::uint32_t centralDirSize = static_cast<std::uint32_t>(os.tellp()) - centralDirOffset;

    // End of central directory
    write_u32(os, 0x06054b50);
    write_u16(os, 0); // disk
    write_u16(os, 0); // disk start
    write_u16(os, static_cast<std::uint16_t>(centrals.size()));
    write_u16(os, static_cast<std::uint16_t>(centrals.size()));
    write_u32(os, centralDirSize);
    write_u32(os, centralDirOffset);
    write_u16(os, 0); // comment length

    os.flush();
    return os.good();
  } catch (const std::exception& ex) {
    if (outErr) *outErr = ex.what();
    return false;
  }
}

} // namespace gf::core
