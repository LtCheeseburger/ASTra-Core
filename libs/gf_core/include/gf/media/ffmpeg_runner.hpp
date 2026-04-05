#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// gf::media::FfmpegRunner
//
// Thin wrapper around an FFmpeg subprocess.
//
// Assumes FFmpeg is available on PATH.  Spawns a child process, captures
// combined stdout+stderr, and returns success/failure + the captured output.
//
// No FFmpeg library is embedded — this is a process launcher only.
// ─────────────────────────────────────────────────────────────────────────────

#include <string>

namespace gf::media {

class FfmpegRunner {
public:
    // Convert inputPath to an MP4 at outputPath.
    //
    // Default command:
    //   ffmpeg -y -i <input> -c:v libx264 -pix_fmt yuv420p -c:a aac <output>
    //
    // When inputFormat is non-empty it is passed as a forced demuxer:
    //   ffmpeg -y -f <inputFormat> -i <input> ...
    //
    // Use inputFormat = "ea" for EA VP6 containers to force the EA demuxer.
    //
    // Returns true if FFmpeg exits with code 0.
    // On failure, errorOut receives the captured stdout+stderr text.
    //
    // Possible failures:
    //   - FFmpeg not found on PATH ("Could not launch FFmpeg")
    //   - FFmpeg reports a stream error ("FFmpeg failed: ...")
    //   - Output file could not be written
    static bool run(const std::string& inputPath,
                    const std::string& outputPath,
                    std::string&       errorOut,
                    const std::string& inputFormat = "");
};

} // namespace gf::media
