#include "gf/media/ea_vp6_import_prep.hpp"
#include "gf/media/ea_vp6.hpp"
#include "gf/media/vp6_encoder_backend.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#ifndef _WIN32
#  include <sys/wait.h>
#endif

#ifdef _WIN32
#  define GF_POPEN  _popen
#  define GF_PCLOSE _pclose
#else
#  define GF_POPEN  popen
#  define GF_PCLOSE pclose
#endif

namespace gf::media {

// ─────────────────────────────────────────────────────────────────────────────
// Internal subprocess helpers
// ─────────────────────────────────────────────────────────────────────────────

// Run cmd, capture stdout+stderr (2>&1 expected in cmd), return captured text.
// Returns std::nullopt if the process could not be launched.
// exitCode is set to the process exit code on success (0 = ok).
static std::optional<std::string> runCapture(const std::string& cmd, int& exitCode)
{
    FILE* pipe = GF_POPEN(cmd.c_str(), "r");
    if (!pipe) return std::nullopt;

    std::string captured;
    std::array<char, 512> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        captured += buf.data();

    const int rc = GF_PCLOSE(pipe);
#ifdef _WIN32
    exitCode = rc;
#else
    exitCode = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
    return captured;
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal string helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string toLower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Parse a fraction string like "30/1" or "30000/1001".
// Returns 0.0 on any parse error or division by zero.
static double parseFraction(const std::string& s)
{
    try {
        const auto pos = s.find('/');
        if (pos == std::string::npos)
            return std::stod(s);
        const double num = std::stod(s.substr(0, pos));
        const double den = std::stod(s.substr(pos + 1));
        return (den != 0.0) ? num / den : 0.0;
    } catch (...) {
        return 0.0;
    }
}

// Safely get a value from a JSON object, returning a default if missing or
// the wrong type.
template<typename T>
static T jsonGet(const nlohmann::json& obj, const std::string& key, T def) noexcept
{
    try {
        if (obj.contains(key) && !obj[key].is_null())
            return obj[key].get<T>();
    } catch (...) {}
    return def;
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. Original EA VP6 target analysis
// ─────────────────────────────────────────────────────────────────────────────

std::optional<EaVp6ImportTarget>
analyzeEaVp6ImportTarget(std::span<const std::uint8_t> originalEaVp6Bytes,
                         std::string& errorOut)
{
    EaVp6Parser parser;
    std::string parseErr;
    auto movieOpt = parser.parse(originalEaVp6Bytes, parseErr);
    if (!movieOpt) {
        errorOut = "EA VP6 target analysis: failed to parse original file: " + parseErr;
        return std::nullopt;
    }

    const EaVp6Movie& movie = *movieOpt;
    EaVp6ImportTarget target;

    // Propagate any parser warnings
    target.warnings.insert(target.warnings.end(),
                           movie.warnings.begin(), movie.warnings.end());

    // Pull metadata from the decoded header
    target.headerValid = movie.header.valid;
    if (movie.header.valid) {
        target.width            = movie.header.width;
        target.height           = movie.header.height;
        target.fps              = movie.header.fps;
        target.frameCount       = movie.header.frameCount;
        target.largestChunkSize = movie.header.largestChunkSize;
        target.fpsDenominator   = movie.header.fpsDenominator;
        target.fpsNumerator     = movie.header.fpsNumerator;
        target.codecId          = movie.header.codecId;
    } else {
        target.warnings.push_back(
            "MVhd header is not valid; target dimensions and fps are unknown");
    }

    // Count frame chunks
    for (const auto& chunk : movie.chunks) {
        switch (chunk.kind) {
        case EaVp6ChunkKind::MV0K: ++target.keyframeChunkCount;   break;
        case EaVp6ChunkKind::MV0F: ++target.deltaFrameChunkCount; break;
        default: break;
        }
    }
    target.hasFrameChunks =
        (target.keyframeChunkCount + target.deltaFrameChunkCount) > 0;

    // Sanity warnings
    if (!target.hasFrameChunks)
        target.warnings.push_back(
            "No MV0K or MV0F frame chunks found; file may be incomplete or unsupported");
    if (target.headerValid) {
        if (target.width == 0 || target.height == 0)
            target.warnings.push_back("Target dimensions are zero");
        if (target.fps <= 0.0)
            target.warnings.push_back("Target fps is not available");
        if (target.frameCount == 0)
            target.warnings.push_back("Target frame count is zero");
    }

    return target;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Source video analysis via ffprobe
// ─────────────────────────────────────────────────────────────────────────────

std::optional<VideoSourceInfo>
probeSourceVideoWithFfmpeg(const std::filesystem::path& inputPath,
                           std::string& errorOut)
{
    // Quote the path and redirect stderr so JSON output is clean.
    // ffprobe is expected alongside ffmpeg on PATH.
    //
    // We request streams + format in JSON.  If ffprobe is unavailable the
    // popen will fail and we return std::nullopt.
    const std::string pathStr = inputPath.string();

    // Escape any double quotes in path (rare, but defensive)
    std::string escapedPath;
    escapedPath.reserve(pathStr.size());
    for (char c : pathStr) {
        if (c == '"') escapedPath += '\\';
        escapedPath += c;
    }

    const std::string cmd =
        "ffprobe -v error -print_format json"
        " -show_streams -show_format"
        " \"" + escapedPath + "\" 2>&1";

    int exitCode = -1;
    auto outputOpt = runCapture(cmd, exitCode);
    if (!outputOpt) {
        errorOut = "probeSourceVideoWithFfmpeg: could not launch ffprobe; "
                   "is it installed and on PATH?";
        return std::nullopt;
    }

    if (outputOpt->empty() || exitCode != 0) {
        // Non-zero exit usually means the file couldn't be opened.
        errorOut = "probeSourceVideoWithFfmpeg: ffprobe exited with code " +
                   std::to_string(exitCode) +
                   (outputOpt->empty() ? " (no output)" : ": " + *outputOpt);
        return std::nullopt;
    }

    // ── Parse JSON ────────────────────────────────────────────────────────────
    VideoSourceInfo info;
    try {
        const nlohmann::json root = nlohmann::json::parse(*outputOpt);

        // Format (container)
        if (root.contains("format") && root["format"].is_object()) {
            const auto& fmt = root["format"];
            info.containerName = jsonGet<std::string>(fmt, "format_long_name",
                                    jsonGet<std::string>(fmt, "format_name", ""));
            const std::string durStr = jsonGet<std::string>(fmt, "duration", "");
            if (!durStr.empty()) {
                try { info.durationSec = std::stod(durStr); } catch (...) {}
            }
        }

        // Streams
        if (root.contains("streams") && root["streams"].is_array()) {
            for (const auto& stream : root["streams"]) {
                const std::string codecType =
                    jsonGet<std::string>(stream, "codec_type", "");

                if (codecType == "video" && !info.hasVideo) {
                    // First video stream wins.
                    info.hasVideo    = true;
                    info.videoCodec  = jsonGet<std::string>(stream, "codec_name", "");
                    info.width       = static_cast<std::uint32_t>(
                                           jsonGet<int>(stream, "width", 0));
                    info.height      = static_cast<std::uint32_t>(
                                           jsonGet<int>(stream, "height", 0));

                    // Prefer avg_frame_rate over r_frame_rate (more stable for
                    // variable-frame-rate sources).
                    const std::string avgFps =
                        jsonGet<std::string>(stream, "avg_frame_rate", "");
                    const std::string rFps =
                        jsonGet<std::string>(stream, "r_frame_rate", "");
                    const std::string fpsStr = avgFps.empty() ? rFps : avgFps;
                    if (!fpsStr.empty() && fpsStr != "0/0")
                        info.fps = parseFraction(fpsStr);

                    // Stream-level duration (more precise than format duration)
                    const std::string sDur =
                        jsonGet<std::string>(stream, "duration", "");
                    if (!sDur.empty()) {
                        try { info.durationSec = std::stod(sDur); } catch (...) {}
                    }

                    // nb_frames (if available)
                    const std::string nbF =
                        jsonGet<std::string>(stream, "nb_frames", "");
                    if (!nbF.empty()) {
                        try {
                            info.videoFrameCountEstimate =
                                static_cast<std::uint64_t>(std::stoull(nbF));
                        } catch (...) {}
                    }
                }

                if (codecType == "audio" && !info.hasAudio) {
                    info.hasAudio  = true;
                    info.audioCodec = jsonGet<std::string>(stream, "codec_name", "");
                }
            }
        }

        // Estimate frame count from duration + fps if nb_frames was unavailable
        if (info.videoFrameCountEstimate == 0 &&
            info.fps > 0.0 && info.durationSec > 0.0)
        {
            info.videoFrameCountEstimate =
                static_cast<std::uint64_t>(std::round(info.durationSec * info.fps));
        }

        if (!info.hasVideo)
            info.warnings.push_back("No video stream found in source file");

        info.valid = true;

    } catch (const nlohmann::json::exception& ex) {
        errorOut = "probeSourceVideoWithFfmpeg: failed to parse ffprobe JSON output: " +
                   std::string(ex.what());
        return std::nullopt;
    } catch (const std::exception& ex) {
        errorOut = "probeSourceVideoWithFfmpeg: unexpected error: " +
                   std::string(ex.what());
        return std::nullopt;
    }

    return info;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Normalisation plan
// ─────────────────────────────────────────────────────────────────────────────

// FPS values are considered "materially different" when they differ by more
// than this fraction of the target FPS.
static constexpr double kFpsEpsilonFrac = 0.005;  // 0.5 %

VideoNormalizationPlan
buildNormalizationPlan(const EaVp6ImportTarget& target,
                       const VideoSourceInfo&   source)
{
    VideoNormalizationPlan plan;

    // We always reencode for VP6 prep.
    plan.reencodeRequired = true;
    plan.steps.push_back("Re-encode video (required for VP6 container format)");

    // ── Output dimensions ─────────────────────────────────────────────────────
    if (target.headerValid && target.width > 0 && target.height > 0) {
        plan.outputWidth  = target.width;
        plan.outputHeight = target.height;

        if (source.valid && source.hasVideo &&
            (source.width != target.width || source.height != target.height))
        {
            plan.resizeRequired = true;
            plan.steps.push_back(
                "Resize " +
                std::to_string(source.width) + "x" + std::to_string(source.height) +
                " -> " +
                std::to_string(target.width) + "x" + std::to_string(target.height));
        }
    } else if (source.valid && source.hasVideo && source.width > 0 && source.height > 0) {
        // Target dimensions unknown — preserve source as best effort.
        plan.outputWidth  = source.width;
        plan.outputHeight = source.height;
        plan.warnings.push_back(
            "Target dimensions unknown; output will match source ("  +
            std::to_string(source.width) + "x" + std::to_string(source.height) + ")");
    } else {
        plan.warnings.push_back(
            "Neither target nor source dimensions are available; plan is incomplete");
    }

    // ── Output FPS ────────────────────────────────────────────────────────────
    if (target.headerValid && target.fps > 0.0) {
        plan.outputFps = target.fps;

        if (source.valid && source.fps > 0.0) {
            const double diff = std::abs(source.fps - target.fps);
            if (diff > target.fps * kFpsEpsilonFrac) {
                plan.fpsConversionRequired = true;

                std::ostringstream fps_ss;
                fps_ss << std::fixed << std::setprecision(2)
                       << "Convert frame rate " << source.fps
                       << " -> " << target.fps;
                plan.steps.push_back(fps_ss.str());
            }
        }
    } else if (source.valid && source.fps > 0.0) {
        plan.outputFps = source.fps;
        plan.warnings.push_back(
            "Target fps unknown; output will preserve source fps (" +
            [&](){ std::ostringstream ss; ss << std::fixed << std::setprecision(2) << source.fps; return ss.str(); }() + ")");
    }

    // ── Audio ─────────────────────────────────────────────────────────────────
    // MVP: always strip audio.  EA VP6 audio handling is not yet implemented.
    plan.stripAudio = true;
    plan.steps.push_back("Strip audio (EA VP6 audio import not yet supported)");

    // ── Pixel format ─────────────────────────────────────────────────────────
    // VP6 requires YUV 4:2:0.
    plan.steps.push_back("Convert pixel format to yuv420p");

    // ── Validity ──────────────────────────────────────────────────────────────
    plan.valid = (plan.outputWidth > 0 && plan.outputHeight > 0);

    return plan;
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. VP6 encoder feasibility probe
// ─────────────────────────────────────────────────────────────────────────────

Vp6EncoderProbeResult probeVp6EncoderSupport()
{
    Vp6EncoderProbeResult result;

    // Run "ffmpeg -encoders" and capture combined output.
    // We send 2>&1 so that the version banner (on stderr) doesn't interfere
    // with the encoder list (on stdout).
    int exitCode = -1;
    auto outputOpt = runCapture("ffmpeg -encoders 2>&1", exitCode);
    if (!outputOpt) {
        result.support = Vp6EncoderSupport::NotAvailable;
        result.warnings.push_back(
            "Could not launch ffmpeg; is it installed and on PATH?");
        return result;
    }

    // Assume NotAvailable until we find a VP6 encoder line.
    result.support = Vp6EncoderSupport::NotAvailable;

    std::istringstream ss(*outputOpt);
    std::string line;
    while (std::getline(ss, line)) {
        // Encoder list lines have format:
        //   " V..... encoder_name   Description"
        //   " A..... encoder_name   Description"
        //
        // We want VIDEO encoders (line[1] == 'V') whose name or description
        // contains "vp6".
        //
        // The header/separator lines do not start with exactly " V" or " A".

        if (line.size() < 8) continue;
        if (line[0] != ' ') continue;

        const char mediaType = line[1];
        if (mediaType != 'V' && mediaType != 'A' && mediaType != 'S') continue;
        // Flags occupy line[1..6]; name starts at roughly line[8].

        const std::string lower = toLower(line);
        if (lower.find("vp6") == std::string::npos) continue;

        // Preserve any VP6-related line for diagnostics.
        result.rawLines.push_back(line);

        // Only count a VIDEO encoder as useful for our purposes.
        if (mediaType != 'V') continue;

        // Extract encoder name: skip past flags (6 chars after the type char)
        // and any whitespace.
        // Format: " V..... name   desc"
        //          0123456789...
        // line[7] is typically a space; name begins at line[8].
        std::string nameToken;
        if (line.size() > 8) {
            std::istringstream tokenStream(line.substr(8));
            tokenStream >> nameToken;
        }

        const std::string lowerName = toLower(nameToken);
        if (!nameToken.empty() && lowerName.find("vp6") != std::string::npos) {
            // First VP6 video encoder wins.
            if (result.support != Vp6EncoderSupport::Available) {
                result.encoderName = nameToken;
                result.support     = Vp6EncoderSupport::Available;
            }
        }
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Combined import-preparation result
// ─────────────────────────────────────────────────────────────────────────────

EaVp6ImportPreparationResult
prepareEaVp6Import(const std::vector<std::uint8_t>& originalEaVp6Bytes,
                   const std::filesystem::path&     sourceVideoPath,
                   std::string&                     errorOut)
{
    EaVp6ImportPreparationResult result;

    // ── 1. Analyse EA VP6 target ─────────────────────────────────────────────
    std::string targetErr;
    auto targetOpt = analyzeEaVp6ImportTarget(
        std::span<const std::uint8_t>(originalEaVp6Bytes.data(),
                                      originalEaVp6Bytes.size()),
        targetErr);
    if (!targetOpt) {
        // Catastrophic: can't even parse the original file.
        errorOut = targetErr;
        result.blockingReasons.push_back(
            "Original EA VP6 file could not be parsed: " + targetErr);
        return result;
    }
    result.target = *targetOpt;

    for (const auto& w : result.target.warnings)
        result.warnings.push_back("[target] " + w);

    if (!result.target.headerValid)
        result.blockingReasons.push_back(
            "Original EA VP6 target has invalid or missing header (dimensions and fps unknown)");
    else {
        if (result.target.width == 0 || result.target.height == 0)
            result.blockingReasons.push_back(
                "Original EA VP6 target has zero dimensions");
        if (result.target.fps <= 0.0)
            result.warnings.push_back("[target] Target fps unavailable; fps will be inferred from source");
    }

    // ── 2. Probe source video ─────────────────────────────────────────────────
    std::string sourceErr;
    auto sourceOpt = probeSourceVideoWithFfmpeg(sourceVideoPath, sourceErr);
    if (!sourceOpt) {
        result.blockingReasons.push_back("Source video could not be probed: " + sourceErr);
        // Still continue so we can run the encoder probe.
    } else {
        result.source = *sourceOpt;
        for (const auto& w : result.source.warnings)
            result.warnings.push_back("[source] " + w);

        if (!result.source.valid || !result.source.hasVideo)
            result.blockingReasons.push_back("Source file has no video stream");
    }

    // ── 3. Build normalisation plan ───────────────────────────────────────────
    result.normalization = buildNormalizationPlan(result.target, result.source);
    for (const auto& w : result.normalization.warnings)
        result.warnings.push_back("[normalization] " + w);

    // ── 4. Probe VP6 encoder ─────────────────────────────────────────────────
    // Check VirtualDub first (preferred), then FFmpeg as fallback.
    {
        const std::filesystem::path vdPath = VirtualDubVp6EncoderBackend::findVirtualDub();
        if (!vdPath.empty()) {
            result.encoderBackendName = "VirtualDub (" + vdPath.filename().string() + ")";
        } else {
            // VirtualDub not found — check FFmpeg VP6 encoder.
            result.encoderProbe = probeVp6EncoderSupport();
            for (const auto& w : result.encoderProbe.warnings)
                result.warnings.push_back("[encoder] " + w);

            if (result.encoderProbe.support == Vp6EncoderSupport::Available &&
                !result.encoderProbe.encoderName.empty())
            {
                result.encoderBackendName = "FFmpeg (" + result.encoderProbe.encoderName + ")";
            } else {
                result.blockingReasons.push_back(
                    "No VP6 encoder available: VirtualDub + On2 VP62 VFW codec not found, "
                    "and FFmpeg has no VP6 encoder in this build. "
                    "Install VirtualDub with the On2 VP6 (VP62) codec, or set VIRTUALDUB_EXE.");
            }
        }
    }

    // ── 5. Determine possibility ──────────────────────────────────────────────
    result.possible = result.blockingReasons.empty();

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Normalisation command builder
// ─────────────────────────────────────────────────────────────────────────────

std::optional<std::vector<std::string>>
buildNormalizationCommand(const VideoNormalizationPlan& plan,
                          const std::filesystem::path&  inputPath,
                          const std::filesystem::path&  outputPath,
                          std::string& errorOut)
{
    if (!plan.valid) {
        errorOut = "buildNormalizationCommand: plan is not valid "
                   "(output dimensions are zero or unresolved)";
        return std::nullopt;
    }
    if (plan.outputWidth == 0 || plan.outputHeight == 0) {
        errorOut = "buildNormalizationCommand: output dimensions are zero";
        return std::nullopt;
    }

    std::vector<std::string> args;
    args.push_back("ffmpeg");
    args.push_back("-y");
    args.push_back("-i");
    args.push_back(inputPath.string());

    // Build -vf filter chain.
    // scale: only when dimensions differ from source.
    // fps:   ALWAYS when outputFps > 0 — enforces a deterministic frame grid
    //        regardless of whether the source nominally matches the target fps.
    std::string vfChain;

    if (plan.resizeRequired) {
        vfChain += "scale=" +
                   std::to_string(plan.outputWidth) + ":" +
                   std::to_string(plan.outputHeight);
    }

    if (plan.outputFps > 0.0) {
        if (!vfChain.empty()) vfChain += ",";
        std::ostringstream fpsSS;
        fpsSS << std::fixed << std::setprecision(6) << plan.outputFps;
        std::string fpsStr = fpsSS.str();
        fpsStr.erase(fpsStr.find_last_not_of('0') + 1);
        if (fpsStr.back() == '.') fpsStr.push_back('0'); // "30.0" not "30."
        vfChain += "fps=" + fpsStr;
    }

    if (!vfChain.empty()) {
        args.push_back("-vf");
        args.push_back(vfChain);
    }

    // Pixel format required for VP6 compatibility.
    args.push_back("-pix_fmt");
    args.push_back("yuv420p");

    // Strip audio (EA VP6 audio import not yet supported).
    if (plan.stripAudio)
        args.push_back("-an");

    // Enforce constant frame rate.  Required so frame positions map
    // deterministically to MV0K/MV0F chunk slots.
    args.push_back("-vsync");
    args.push_back("cfr");

    args.push_back(outputPath.string());

    return args;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage B — Normalization execution
// ─────────────────────────────────────────────────────────────────────────────

// Build a shell command string from an argv vector, quoting paths with spaces.
// Appends "2>&1" so stderr is captured alongside stdout.
static std::string buildShellCommand(const std::vector<std::string>& args)
{
    std::string cmd;
    for (const auto& arg : args) {
        if (!cmd.empty()) cmd += ' ';
        const bool needsQuotes = arg.find(' ') != std::string::npos ||
                                 arg.find('"') != std::string::npos;
        if (needsQuotes) {
            cmd += '"';
            for (char c : arg) {
                if (c == '"') cmd += '\\';
                cmd += c;
            }
            cmd += '"';
        } else {
            cmd += arg;
        }
    }
    cmd += " 2>&1";
    return cmd;
}

// Populate NormalizedVideoInfo from a VideoSourceInfo probe result.
static NormalizedVideoInfo normalizedInfoFromProbe(const VideoSourceInfo&        probe,
                                                    const std::filesystem::path& filePath)
{
    NormalizedVideoInfo info;
    info.filePath    = filePath;
    info.kind        = IntermediateVideoKind::NormalizedMp4;
    info.width       = probe.width;
    info.height      = probe.height;
    info.fps         = probe.fps;
    info.durationSec = probe.durationSec;
    info.hasAudio    = probe.hasAudio;
    info.warnings    = probe.warnings;

    // Prefer nb_frames (already computed by probeSourceVideoWithFfmpeg into
    // videoFrameCountEstimate, which is either the literal nb_frames value or
    // round(duration × fps)).
    if (probe.videoFrameCountEstimate > 0) {
        if (probe.videoFrameCountEstimate <=
                static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            info.frameCount = static_cast<std::uint32_t>(probe.videoFrameCountEstimate);
        } else {
            info.frameCount = std::numeric_limits<std::uint32_t>::max();
            info.warnings.push_back(
                "Frame count exceeds uint32 maximum; capped at 4294967295");
        }
    } else if (probe.fps > 0.0 && probe.durationSec > 0.0) {
        // Final fallback: compute from duration if neither nb_frames nor the
        // existing estimate is available.
        const double est = std::round(probe.durationSec * probe.fps);
        info.frameCount = static_cast<std::uint32_t>(
            std::min(est, static_cast<double>(
                std::numeric_limits<std::uint32_t>::max())));
    }

    info.valid = probe.valid && probe.hasVideo;
    return info;
}

// Format a double fps value as a short string for diagnostics.
static std::string fmtFps(double fps)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(3) << fps;
    std::string s = ss.str();
    // Trim trailing zeros but keep at least one decimal place.
    const auto dot = s.find('.');
    if (dot != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (s.back() == '.') s += '0';
    }
    return s;
}

// ── 9. executeNormalization ───────────────────────────────────────────────────

VideoNormalizationExecutionResult
executeNormalization(const VideoNormalizationPlan& plan,
                     const EaVp6ImportTarget&      target,
                     const std::filesystem::path&  inputPath,
                     const std::filesystem::path&  outputPath)
{
    VideoNormalizationExecutionResult result;
    result.plan       = plan;
    result.outputPath = outputPath;

    // ── Step 1: build command ─────────────────────────────────────────────────
    std::string cmdErr;
    auto argsOpt = buildNormalizationCommand(plan, inputPath, outputPath, cmdErr);
    if (!argsOpt) {
        result.errors.push_back("Could not build FFmpeg command: " + cmdErr);
        return result;
    }

    result.ffmpegCommandSummary = buildShellCommand(*argsOpt);
    // Remove the trailing " 2>&1" from the display summary; it's an implementation
    // detail.  We keep the shell command for the actual run below.
    {
        const std::string suffix = " 2>&1";
        if (result.ffmpegCommandSummary.size() > suffix.size())
            result.ffmpegCommandSummary.resize(
                result.ffmpegCommandSummary.size() - suffix.size());
    }

    // ── Step 2: run FFmpeg ────────────────────────────────────────────────────
    const std::string shellCmd = buildShellCommand(*argsOpt);

    int exitCode = -1;
    auto capturedOpt = runCapture(shellCmd, exitCode);

    if (!capturedOpt) {
        result.errors.push_back(
            "Could not launch FFmpeg; is it installed and on PATH?");
        return result;
    }

    result.ffmpegStdoutStderr = *capturedOpt;

    if (exitCode != 0) {
        result.errors.push_back(
            "FFmpeg exited with code " + std::to_string(exitCode) +
            "; see ffmpegStdoutStderr for details");
        return result;
    }

    // ── Step 3: probe the output ──────────────────────────────────────────────
    std::string probeErr;
    auto probeOpt = probeSourceVideoWithFfmpeg(outputPath, probeErr);

    if (!probeOpt) {
        result.errors.push_back(
            "Output file could not be probed after normalization: " + probeErr);
        return result;
    }

    result.normalizedInfo = normalizedInfoFromProbe(*probeOpt, outputPath);

    if (!result.normalizedInfo.valid) {
        result.errors.push_back(
            "Normalized output has no valid video stream");
        return result;
    }

    // ── Step 4: validate output against target ────────────────────────────────
    bool allValid = true;

    // Dimensions must match exactly.
    if (target.headerValid &&
        (result.normalizedInfo.width  != target.width ||
         result.normalizedInfo.height != target.height))
    {
        result.errors.push_back(
            "Normalized dimensions " +
            std::to_string(result.normalizedInfo.width) + "x" +
            std::to_string(result.normalizedInfo.height) +
            " do not match target " +
            std::to_string(target.width) + "x" + std::to_string(target.height));
        allValid = false;
    }

    // FPS must match within tolerance.
    if (target.headerValid && target.fps > 0.0 && result.normalizedInfo.fps > 0.0) {
        const double diff = std::abs(result.normalizedInfo.fps - target.fps);
        if (diff > target.fps * kFpsEpsilonFrac) {
            result.errors.push_back(
                "Normalized fps " + fmtFps(result.normalizedInfo.fps) +
                " does not match target " + fmtFps(target.fps) +
                " (tolerance " + std::to_string(static_cast<int>(kFpsEpsilonFrac * 100)) +
                "%)");
            allValid = false;
        }
    }

    // Frame count must be non-zero.
    if (result.normalizedInfo.frameCount == 0) {
        result.errors.push_back("Normalized output has zero detected frames");
        allValid = false;
    }

    // Audio must be absent.
    if (result.normalizedInfo.hasAudio) {
        result.errors.push_back(
            "Normalized output unexpectedly contains an audio stream; "
            "check FFmpeg -an flag");
        allValid = false;
    }

    // ── Step 5: frame count alignment with target (warning only) ─────────────
    if (target.frameCount > 0 && result.normalizedInfo.frameCount > 0) {
        result.frameCountMatchesTarget =
            (result.normalizedInfo.frameCount == target.frameCount);

        if (!result.frameCountMatchesTarget) {
            result.warnings.push_back(
                "Normalized frame count differs from original EA VP6 "
                "(target=" + std::to_string(target.frameCount) +
                ", normalized=" + std::to_string(result.normalizedInfo.frameCount) + ")");
        }
    }

    result.success = allValid;
    return result;
}

// ── 10. runNormalizationStage ─────────────────────────────────────────────────

EaVp6NormalizationStageResult
runNormalizationStage(const std::vector<std::uint8_t>& originalEaVp6Bytes,
                      const std::filesystem::path&     sourceVideoPath,
                      const std::filesystem::path&     outputVideoPath,
                      std::string&                     errorOut)
{
    EaVp6NormalizationStageResult result;

    // ── 1. Analyse EA VP6 target ──────────────────────────────────────────────
    std::string targetErr;
    auto targetOpt = analyzeEaVp6ImportTarget(
        std::span<const std::uint8_t>(originalEaVp6Bytes.data(),
                                      originalEaVp6Bytes.size()),
        targetErr);
    if (!targetOpt) {
        errorOut = targetErr;
        result.blockingReasons.push_back(
            "Original EA VP6 file could not be parsed: " + targetErr);
        return result;
    }
    result.target = *targetOpt;

    for (const auto& w : result.target.warnings)
        result.warnings.push_back("[target] " + w);

    if (!result.target.headerValid) {
        result.blockingReasons.push_back(
            "Original EA VP6 target has invalid or missing MVhd header "
            "(dimensions and fps unknown — cannot normalize)");
        return result;
    }
    if (result.target.width == 0 || result.target.height == 0) {
        result.blockingReasons.push_back(
            "Original EA VP6 target has zero dimensions");
        return result;
    }

    // ── 2. Probe source video ─────────────────────────────────────────────────
    std::string sourceErr;
    auto sourceOpt = probeSourceVideoWithFfmpeg(sourceVideoPath, sourceErr);
    if (!sourceOpt) {
        result.blockingReasons.push_back(
            "Source video could not be probed: " + sourceErr);
        return result;
    }
    result.source = *sourceOpt;

    for (const auto& w : result.source.warnings)
        result.warnings.push_back("[source] " + w);

    if (!result.source.valid || !result.source.hasVideo) {
        result.blockingReasons.push_back("Source file has no video stream");
        return result;
    }

    // ── 3. Build normalization plan ───────────────────────────────────────────
    result.plan = buildNormalizationPlan(result.target, result.source);

    for (const auto& w : result.plan.warnings)
        result.warnings.push_back("[plan] " + w);

    if (!result.plan.valid) {
        result.blockingReasons.push_back(
            "Normalization plan is invalid (output dimensions could not be determined)");
        return result;
    }

    // ── 4. Execute normalization ──────────────────────────────────────────────
    result.execution = executeNormalization(
        result.plan, result.target, sourceVideoPath, outputVideoPath);

    for (const auto& w : result.execution.warnings)
        result.warnings.push_back("[normalization] " + w);

    if (!result.execution.success) {
        for (const auto& e : result.execution.errors)
            result.blockingReasons.push_back("[normalization] " + e);
        return result;
    }

    // ── 5. Determine readyForEncoding ────────────────────────────────────────
    // All structural checks passed inside executeNormalization — if we reach
    // here, success is true and validation succeeded.
    result.readyForEncoding = true;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. Diagnostic summary (Stage A)
// ─────────────────────────────────────────────────────────────────────────────

std::string describeImportPreparation(const EaVp6ImportPreparationResult& result)
{
    std::ostringstream ss;
    ss << "EA VP6 Import Preparation\n";
    ss << "- Possible: " << (result.possible ? "yes" : "no") << "\n";

    // Target
    {
        const auto& t = result.target;
        if (t.headerValid && t.width > 0 && t.height > 0) {
            ss << "- Target: " << t.width << "x" << t.height;
            if (t.fps > 0.0)
                ss << " @ " << std::fixed << std::setprecision(2) << t.fps << " fps";
            ss << ", " << t.frameCount << " frames";
            ss << " (K=" << t.keyframeChunkCount
               << " F=" << t.deltaFrameChunkCount << ")\n";
        } else {
            ss << "- Target: (header invalid or dimensions unknown)\n";
        }
    }

    // Source
    {
        const auto& s = result.source;
        if (s.valid && s.hasVideo) {
            ss << "- Source: " << s.width << "x" << s.height;
            if (s.fps > 0.0)
                ss << " @ " << std::fixed << std::setprecision(2) << s.fps << " fps";
            if (s.durationSec > 0.0)
                ss << ", duration " << std::fixed << std::setprecision(2)
                   << s.durationSec << "s";
            if (!s.videoCodec.empty())
                ss << ", codec=" << s.videoCodec;
            ss << "\n";
        } else if (!s.containerName.empty()) {
            ss << "- Source: found (" << s.containerName << ") but no video stream\n";
        } else {
            ss << "- Source: (not probed or probe failed)\n";
        }
    }

    // Normalization
    {
        const auto& n = result.normalization;
        if (n.valid && !n.steps.empty()) {
            ss << "- Normalization:\n";
            for (const auto& step : n.steps)
                ss << "  * " << step << "\n";
        }
    }

    // Encoder
    {
        ss << "- Encoder: ";
        if (!result.encoderBackendName.empty())
            ss << result.encoderBackendName << "\n";
        else
            ss << "VP6 encoder not available\n";
    }

    // Warnings
    if (!result.warnings.empty()) {
        ss << "- Warnings:\n";
        for (const auto& w : result.warnings)
            ss << "  ! " << w << "\n";
    }

    // Blocking reasons
    if (!result.blockingReasons.empty()) {
        ss << "- Blocking reasons:\n";
        for (const auto& r : result.blockingReasons)
            ss << "  * " << r << "\n";
    }

    return ss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// 11. Diagnostic summary (Stage B)
// ─────────────────────────────────────────────────────────────────────────────

std::string describeNormalizationStage(const EaVp6NormalizationStageResult& result)
{
    std::ostringstream ss;
    ss << "EA VP6 Normalization Stage\n";
    ss << "- Ready for encoding: " << (result.readyForEncoding ? "yes" : "no") << "\n";

    // Target
    {
        const auto& t = result.target;
        if (t.headerValid && t.width > 0 && t.height > 0) {
            ss << "- Target: " << t.width << "x" << t.height;
            if (t.fps > 0.0)
                ss << " @ " << fmtFps(t.fps) << " fps";
            ss << ", " << t.frameCount << " frames";
            ss << " (K=" << t.keyframeChunkCount
               << " F=" << t.deltaFrameChunkCount << ")\n";
        } else {
            ss << "- Target: (header invalid or dimensions unknown)\n";
        }
    }

    // Source
    {
        const auto& s = result.source;
        if (s.valid && s.hasVideo) {
            ss << "- Source: " << s.width << "x" << s.height;
            if (s.fps > 0.0)
                ss << " @ " << fmtFps(s.fps) << " fps";
            if (s.durationSec > 0.0)
                ss << ", duration " << std::fixed << std::setprecision(2)
                   << s.durationSec << "s";
            if (!s.videoCodec.empty())
                ss << ", codec=" << s.videoCodec;
            ss << "\n";
        } else {
            ss << "- Source: (not probed or no video stream)\n";
        }
    }

    // Plan steps
    {
        const auto& p = result.plan;
        if (p.valid && !p.steps.empty()) {
            ss << "- Plan:\n";
            for (const auto& step : p.steps)
                ss << "  * " << step << "\n";
        }
    }

    // Execution
    {
        const auto& ex = result.execution;
        if (!ex.ffmpegCommandSummary.empty()) {
            ss << "- FFmpeg command: " << ex.ffmpegCommandSummary << "\n";
        }
        if (ex.success) {
            const auto& ni = ex.normalizedInfo;
            ss << "- Normalized output: " << ni.width << "x" << ni.height;
            if (ni.fps > 0.0)
                ss << " @ " << fmtFps(ni.fps) << " fps";
            ss << ", " << ni.frameCount << " frames";
            if (ni.durationSec > 0.0)
                ss << ", duration " << std::fixed << std::setprecision(2)
                   << ni.durationSec << "s";
            ss << "\n";
            ss << "- Frame count match: "
               << (ex.frameCountMatchesTarget ? "yes" : "no (see warnings)") << "\n";
        } else if (!ex.errors.empty()) {
            ss << "- Normalization failed:\n";
            for (const auto& e : ex.errors)
                ss << "  ! " << e << "\n";
        }
    }

    // Warnings
    if (!result.warnings.empty()) {
        ss << "- Warnings:\n";
        for (const auto& w : result.warnings)
            ss << "  ! " << w << "\n";
    }

    // Blocking reasons
    if (!result.blockingReasons.empty()) {
        ss << "- Blocking reasons:\n";
        for (const auto& r : result.blockingReasons)
            ss << "  * " << r << "\n";
    }

    return ss.str();
}

} // namespace gf::media
