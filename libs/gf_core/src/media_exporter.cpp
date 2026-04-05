#include "gf/media/media_exporter.hpp"
#include "gf/media/ffmpeg_runner.hpp"

#include <cstdio>
#include <fstream>

namespace gf::media {

// ── extractRaw ────────────────────────────────────────────────────────────────

bool extractRaw(const std::vector<std::uint8_t>& data,
                const std::filesystem::path&     outPath)
{
    std::ofstream ofs(outPath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return false;
    ofs.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    return ofs.good();
}

// ── exportAsMp4 ───────────────────────────────────────────────────────────────

bool exportAsMp4(const std::vector<std::uint8_t>& data,
                 const MediaProbeResult&           probe,
                 const std::filesystem::path&      outPath,
                 std::string&                      errorOut)
{
    switch (probe.kind) {

    case MediaKind::Mp4:
        // Already a valid MP4 — write directly, no transcoding.
        if (!extractRaw(data, outPath)) {
            errorOut = "Could not write output file: " + outPath.string();
            return false;
        }
        return true;

    case MediaKind::EaVp6Container: {
        // Write a temp file alongside the output, transcode via FFmpeg, then clean up.
        std::filesystem::path tempPath = outPath;
        tempPath.replace_extension(".vp6ea.tmp");

        if (!extractRaw(data, tempPath)) {
            errorOut = "Could not write temporary file: " + tempPath.string();
            return false;
        }

        // Force the EA demuxer so FFmpeg recognises the proprietary container
        // regardless of the temp file's extension.
        const bool ok = FfmpegRunner::run(tempPath.string(), outPath.string(), errorOut, "ea");
        std::filesystem::remove(tempPath); // best-effort cleanup regardless of outcome
        return ok;
    }

    case MediaKind::Unknown:
    default:
        errorOut = "Cannot export as MP4: unrecognised media format. "
                   "Use raw extraction instead.";
        return false;
    }
}

} // namespace gf::media
