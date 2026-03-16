#include <gf/core/safe_write.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace gf::core {

static std::string timestamp_suffix() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t t = system_clock::to_time_t(now);

  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif

  std::ostringstream oss;
  oss << ".bak_" << std::put_time(&tm, "%Y%m%d_%H%M%S");
  return oss.str();
}

static std::string random_token() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<std::uint64_t> dist;
  const std::uint64_t v = dist(gen);
  std::ostringstream oss;
  oss << std::hex << v;
  return oss.str();
}

static SafeWriteResult fail(std::string msg,
                           std::optional<std::filesystem::path> backup = std::nullopt,
                           std::optional<std::filesystem::path> tmp = std::nullopt) {
  SafeWriteResult r;
  r.ok = false;
  r.message = std::move(msg);
  r.backup_path = std::move(backup);
  r.temp_path = std::move(tmp);
  return r;
}

SafeWriteResult safe_write_bytes(const std::filesystem::path& target,
                                std::span<const std::byte> data,
                                const SafeWriteOptions& opt) {
  try {
    if (opt.max_bytes != 0 && static_cast<std::uint64_t>(data.size()) > opt.max_bytes) {
      return fail("Refusing to write: data exceeds max_bytes safety limit.");
    }

    const auto parent = target.parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(parent, ec);
      if (ec) {
        return fail("Failed to create parent directories: " + ec.message());
      }
    }

    const std::string tmpName = target.filename().string() + ".tmp_" + random_token();
    const std::filesystem::path tmpPath = target.parent_path() / tmpName;

    {
      std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
      if (!out) {
        return fail("Failed to open temp file for writing.", std::nullopt, tmpPath);
      }
      if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
      }
      out.flush();
      if (!out) {
        return fail("Failed while writing temp file.", std::nullopt, tmpPath);
      }
    }

    // Backup existing file (optional)
    std::optional<std::filesystem::path> backupPath;
    if (opt.make_backup) {
      std::error_code ec;
      if (std::filesystem::exists(target, ec) && !ec) {
        const std::string suffix = opt.backup_suffix.empty() ? timestamp_suffix() : opt.backup_suffix;
        backupPath = target;
        backupPath->replace_filename(target.filename().string() + suffix);

        // Best-effort: if backup already exists, add randomness.
        if (std::filesystem::exists(*backupPath, ec) && !ec) {
          backupPath->replace_filename(target.filename().string() + suffix + "_" + random_token());
        }

        std::filesystem::rename(target, *backupPath, ec);
        if (ec) {
          // If we can't create a backup, do not proceed (avoid destructive overwrite).
          std::filesystem::remove(tmpPath, ec);
          return fail("Failed to create backup: " + ec.message());
        }
      }
    }

    // Replace target with temp.
    {
      std::error_code ec;
      std::filesystem::rename(tmpPath, target, ec);
      if (ec) {
        // Attempt rollback.
        if (backupPath) {
          std::error_code ec2;
          std::filesystem::rename(*backupPath, target, ec2);
        }
        // Best-effort cleanup.
        std::filesystem::remove(tmpPath, ec);
        return fail("Failed to replace destination: " + ec.message(), backupPath, tmpPath);
      }
    }

    SafeWriteResult ok;
    ok.ok = true;
    ok.message = "OK";
    ok.backup_path = backupPath;
    ok.temp_path = std::nullopt;
    return ok;

  } catch (const std::exception& e) {
    return fail(std::string("Exception during safe write: ") + e.what());
  }
}

SafeWriteResult safe_write_text(const std::filesystem::path& target,
                               std::string_view text,
                               const SafeWriteOptions& opt) {
  const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
  return safe_write_bytes(target, std::span<const std::byte>(bytes, text.size()), opt);
}

SafeWriteResult safe_write_streamed(const std::filesystem::path& target,
                                    const SafeWriteCallback&     writer,
                                    const SafeWriteOptions&      opt) {
  try {
    const auto parent = target.parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(parent, ec);
      if (ec) return fail("Failed to create parent directories: " + ec.message());
    }

    const std::string tmpName = target.filename().string() + ".tmp_" + random_token();
    const std::filesystem::path tmpPath = target.parent_path() / tmpName;

    // Write via callback into the temp file.
    {
      std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
      if (!out) return fail("Failed to open temp file for writing.", std::nullopt, tmpPath);

      std::string cbErr;
      if (!writer(out, cbErr)) {
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
        return fail(cbErr.empty() ? "Stream write callback failed." : cbErr);
      }

      out.flush();
      if (!out) {
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
        return fail("Failed to flush temp file after stream write.");
      }
    }

    // Enforce max_bytes limit by checking the actual written file size.
    if (opt.max_bytes != 0) {
      std::error_code ec;
      const auto written = std::filesystem::file_size(tmpPath, ec);
      if (ec || written > opt.max_bytes) {
        std::filesystem::remove(tmpPath, ec);
        return fail("Refusing to commit: written file exceeds max_bytes safety limit.");
      }
    }

    // Create backup of existing destination (optional).
    std::optional<std::filesystem::path> backupPath;
    if (opt.make_backup) {
      std::error_code ec;
      if (std::filesystem::exists(target, ec) && !ec) {
        const std::string suffix = opt.backup_suffix.empty() ? timestamp_suffix() : opt.backup_suffix;
        backupPath = target;
        backupPath->replace_filename(target.filename().string() + suffix);
        if (std::filesystem::exists(*backupPath, ec) && !ec)
          backupPath->replace_filename(target.filename().string() + suffix + "_" + random_token());
        std::filesystem::rename(target, *backupPath, ec);
        if (ec) {
          std::filesystem::remove(tmpPath, ec);
          return fail("Failed to create backup: " + ec.message());
        }
      }
    }

    // Promote temp → target.
    {
      std::error_code ec;
      std::filesystem::rename(tmpPath, target, ec);
      if (ec) {
        if (backupPath) {
          std::error_code ec2;
          std::filesystem::rename(*backupPath, target, ec2);
        }
        std::filesystem::remove(tmpPath, ec);
        return fail("Failed to replace destination: " + ec.message(), backupPath, tmpPath);
      }
    }

    SafeWriteResult ok;
    ok.ok = true;
    ok.message = "OK";
    ok.backup_path = backupPath;
    ok.temp_path = std::nullopt;
    return ok;

  } catch (const std::exception& e) {
    return fail(std::string("Exception during safe streamed write: ") + e.what());
  }
}

} // namespace gf::core
