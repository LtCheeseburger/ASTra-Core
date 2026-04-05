#include "gf/media/ffmpeg_runner.hpp"

#include <array>
#include <cstdio>
#include <string>

#ifdef _WIN32
#  define POPEN  _popen
#  define PCLOSE _pclose
#else
#  define POPEN  popen
#  define PCLOSE pclose
#endif

namespace gf::media {

bool FfmpegRunner::run(const std::string& inputPath,
                       const std::string& outputPath,
                       std::string&       errorOut,
                       const std::string& inputFormat)
{
    // Build command: capture combined stdout+stderr via 2>&1
    // -y                            : overwrite output without prompting
    // -f <inputFormat>              : force demuxer (e.g. "ea" for EA VP6 containers)
    // -c:v libx264 -pix_fmt yuv420p : broad-compatibility H.264
    // -c:a aac                      : AAC audio
    std::string cmd = "ffmpeg -y";
    if (!inputFormat.empty())
        cmd += " -f " + inputFormat;
    cmd += " -i \"" + inputPath + "\""
           " -c:v libx264 -pix_fmt yuv420p -c:a aac"
           " \"" + outputPath + "\" 2>&1";

    FILE* pipe = POPEN(cmd.c_str(), "r");
    if (!pipe) {
        errorOut = "Could not launch FFmpeg. Is it installed and on PATH?";
        return false;
    }

    std::string captured;
    std::array<char, 256> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        captured += buf.data();

    const int rc = PCLOSE(pipe);

#ifdef _WIN32
    const bool success = (rc == 0);
#else
    // POSIX: WIFEXITED + WEXITSTATUS
    const bool success = (WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
#endif

    if (!success) {
        errorOut = "FFmpeg failed:\n" + captured;
        return false;
    }

    return true;
}

} // namespace gf::media
