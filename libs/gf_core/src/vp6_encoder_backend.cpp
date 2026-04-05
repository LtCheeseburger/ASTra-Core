#include "gf/media/vp6_encoder_backend.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>

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
// Internal subprocess helper
// (same pattern as ea_vp6_import_prep.cpp — runCapture is static there)
// ─────────────────────────────────────────────────────────────────────────────

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
// Internal: build a quoted shell command from an argv vector.
// Arguments containing spaces or quotes are double-quoted.
// Appends " 2>&1" for stdout+stderr capture.
// ─────────────────────────────────────────────────────────────────────────────

static std::string argsToShellCmd(const std::vector<std::string>& args)
{
    std::string cmd;
    for (const auto& arg : args) {
        if (!cmd.empty()) cmd += ' ';
        const bool needsQuotes = arg.empty() ||
                                 arg.find(' ')  != std::string::npos ||
                                 arg.find('"')  != std::string::npos;
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

// Internal: human-readable argv summary (space-joined, paths quoted, no "2>&1").
static std::string argsSummary(const std::vector<std::string>& args)
{
    std::string s;
    for (const auto& arg : args) {
        if (!s.empty()) s += ' ';
        if (arg.find(' ') != std::string::npos) {
            s += '"';
            s += arg;
            s += '"';
        } else {
            s += arg;
        }
    }
    return s;
}

// Internal: FPS tolerance fraction (0.5 %) — same as normalization layer.
static constexpr double kFpsEpsilonFrac = 0.005;

// Internal: format fps for display.
static std::string fmtFps(double fps)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(3) << fps;
    std::string s = ss.str();
    const auto dot = s.find('.');
    if (dot != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (s.back() == '.') s += '0';
    }
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// FfmpegVp6EncoderBackend
// ─────────────────────────────────────────────────────────────────────────────

FfmpegVp6EncoderBackend::FfmpegVp6EncoderBackend(Vp6EncoderProbeResult probeResult)
    : m_probe(std::move(probeResult))
{}

Vp6EncoderBackendKind FfmpegVp6EncoderBackend::kind() const
{
    return Vp6EncoderBackendKind::Ffmpeg;
}

bool FfmpegVp6EncoderBackend::available() const
{
    return m_probe.support == Vp6EncoderSupport::Available &&
           !m_probe.encoderName.empty();
}

std::string FfmpegVp6EncoderBackend::displayName() const
{
    if (available())
        return "FFmpeg (" + m_probe.encoderName + ")";
    return "FFmpeg (VP6 encoder not available)";
}

Vp6EncodingResult
FfmpegVp6EncoderBackend::encode(const NormalizedVideoInfo&   normalizedInput,
                                 const std::filesystem::path& workingDirectory) const
{
    Vp6EncodingResult result;
    result.backendKind = Vp6EncoderBackendKind::Ffmpeg;

    if (!available()) {
        result.errors.push_back(
            "FFmpeg VP6 backend is not available: " + displayName());
        return result;
    }

    if (!normalizedInput.valid) {
        result.errors.push_back(
            "Normalized input is not valid; cannot proceed to VP6 encode");
        return result;
    }

    // Output file: FLV container, which is the natural container for VP6.
    // The future EA containerization step will extract raw VP6 frames from it.
    const std::filesystem::path outputPath = workingDirectory / "vp6_encoded.flv";
    result.outputPath = outputPath;
    result.outputKind = Vp6EncodedOutputKind::ContainerizedVp6;

    // Build argv.
    // - Input is already yuv420p, CFR, correct dimensions, no audio.
    // - We only apply -c:v (VP6 encoder) and -an here; no resize, no fps filter.
    std::vector<std::string> args;
    args.push_back("ffmpeg");
    args.push_back("-y");
    args.push_back("-i");
    args.push_back(normalizedInput.filePath.string());
    args.push_back("-c:v");
    args.push_back(m_probe.encoderName);  // e.g. "vp6f" — from probe, not hardcoded
    args.push_back("-an");                // belt-and-suspenders: input has none anyway
    args.push_back(outputPath.string());

    result.commandSummary = argsSummary(args);
    const std::string shellCmd = argsToShellCmd(args);

    // Execute.
    int exitCode = -1;
    auto capturedOpt = runCapture(shellCmd, exitCode);

    if (!capturedOpt) {
        result.errors.push_back(
            "Could not launch FFmpeg; is it installed and on PATH?");
        return result;
    }

    result.stdoutStderr = *capturedOpt;

    if (exitCode != 0) {
        result.errors.push_back(
            "FFmpeg VP6 encode exited with code " + std::to_string(exitCode) +
            "; see stdoutStderr for details");
        return result;
    }

    // Validate the produced output.
    if (!validateVp6EncoderOutput(outputPath, normalizedInput, result))
        return result;

    result.success = true;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// ExternalVp6EncoderBackend
// ─────────────────────────────────────────────────────────────────────────────

ExternalVp6EncoderBackend::ExternalVp6EncoderBackend(std::filesystem::path toolPath)
    : m_toolPath(std::move(toolPath))
{}

Vp6EncoderBackendKind ExternalVp6EncoderBackend::kind() const
{
    return Vp6EncoderBackendKind::ExternalTool;
}

bool ExternalVp6EncoderBackend::available() const
{
    return !m_toolPath.empty() && std::filesystem::exists(m_toolPath);
}

std::string ExternalVp6EncoderBackend::displayName() const
{
    if (m_toolPath.empty())
        return "External tool (not configured)";
    if (!std::filesystem::exists(m_toolPath))
        return "External tool (not found: " + m_toolPath.string() + ")";
    return "External tool (" + m_toolPath.filename().string() + ")";
}

Vp6EncodingResult
ExternalVp6EncoderBackend::encode(const NormalizedVideoInfo&,
                                   const std::filesystem::path&) const
{
    Vp6EncodingResult result;
    result.backendKind = Vp6EncoderBackendKind::ExternalTool;

    if (!available()) {
        result.errors.push_back(
            "External VP6 encoder tool is not configured or does not exist" +
            (m_toolPath.empty()
                 ? std::string{}
                 : ": " + m_toolPath.string()));
        return result;
    }

    // Tool path is configured and exists, but integration is not yet implemented.
    // This seam allows a future implementation to add a real command here without
    // changing the interface.
    result.errors.push_back(
        "External VP6 encoder backend is not yet implemented; "
        "tool path is configured (" + m_toolPath.string() + ") "
        "but the encode command has not been wired in");
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// VirtualDubVp6EncoderBackend
// ─────────────────────────────────────────────────────────────────────────────

VirtualDubVp6EncoderBackend::VirtualDubVp6EncoderBackend(std::filesystem::path executablePath)
    : m_executablePath(std::move(executablePath))
{}

Vp6EncoderBackendKind VirtualDubVp6EncoderBackend::kind() const
{
    return Vp6EncoderBackendKind::VirtualDub;
}

bool VirtualDubVp6EncoderBackend::available() const
{
    return !m_executablePath.empty() && std::filesystem::exists(m_executablePath);
}

std::string VirtualDubVp6EncoderBackend::displayName() const
{
    if (m_executablePath.empty())
        return "VirtualDub (not found)";
    if (!std::filesystem::exists(m_executablePath))
        return "VirtualDub (not found: " + m_executablePath.string() + ")";
    return "VirtualDub (" + m_executablePath.filename().string() + ")";
}

// Escape a path for use inside a VDScript double-quoted string literal.
// VDScript uses C-style string escaping — backslashes and double-quotes
// must be doubled/escaped.
static std::string vdscriptPathLiteral(const std::filesystem::path& p)
{
    const std::string s = p.string();
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\') { out += "\\\\"; }
        else if (c == '"') { out += "\\\""; }
        else { out += c; }
    }
    return out;
}

Vp6EncodingResult
VirtualDubVp6EncoderBackend::encode(const NormalizedVideoInfo&   normalizedInput,
                                     const std::filesystem::path& workingDirectory) const
{
    Vp6EncodingResult result;
    result.backendKind = Vp6EncoderBackendKind::VirtualDub;

    if (!available()) {
        result.errors.push_back(
            "VirtualDub backend is not available: " + displayName());
        return result;
    }

    if (!normalizedInput.valid) {
        result.errors.push_back(
            "Normalized input is not valid; cannot proceed to VP6 encode");
        return result;
    }

    const std::filesystem::path outputPath = workingDirectory / "vd_encoded.avi";
    const std::filesystem::path scriptPath = workingDirectory / "vd_encode.vdscript";
    result.outputPath = outputPath;
    result.outputKind = Vp6EncodedOutputKind::ContainerizedVp6;

    // ── Write VDScript ────────────────────────────────────────────────────────
    // VirtualDub.video.SetCompression(fccHandler, lKey, lDataRate, lQ):
    //   fccHandler = 0x32365056 = MAKEFOURCC('V','P','6','2')  (On2 VP6)
    //   lKey       = 0          — keyframe interval: codec default
    //   lDataRate  = 0          — quality mode (not constant bitrate)
    //   lQ         = 8500       — 85% quality (range 0–10000)
    {
        std::ofstream script(scriptPath);
        if (!script.is_open()) {
            result.errors.push_back(
                "Could not write VDScript to: " + scriptPath.string());
            return result;
        }
        script
            << "// ASTra VP62 encode script\n"
            << "VirtualDub.Open(\""
            << vdscriptPathLiteral(normalizedInput.filePath)
            << "\");\n"
            << "VirtualDub.audio.SetMode(0);\n"
            << "VirtualDub.video.SetMode(1);\n"
            << "VirtualDub.video.SetCompression(0x32365056, 0, 0, 8500);\n"
            << "VirtualDub.SaveAVI(\""
            << vdscriptPathLiteral(outputPath)
            << "\");\n"
            << "VirtualDub.Close();\n";
        if (!script) {
            result.errors.push_back(
                "Error writing VDScript to: " + scriptPath.string());
            return result;
        }
    }

    // ── Build command ─────────────────────────────────────────────────────────
    // VirtualDub.exe /s <script> /x
    //   /s <script>  — execute VDScript file
    //   /x           — exit when script finishes (suppress GUI)
    std::vector<std::string> args;
    args.push_back(m_executablePath.string());
    args.push_back("/s");
    args.push_back(scriptPath.string());
    args.push_back("/x");

    result.commandSummary = argsSummary(args);
    const std::string shellCmd = argsToShellCmd(args);

    // ── Execute ───────────────────────────────────────────────────────────────
    int exitCode = -1;
    auto capturedOpt = runCapture(shellCmd, exitCode);

    if (!capturedOpt) {
        result.errors.push_back(
            "Could not launch VirtualDub; command: " + result.commandSummary);
        return result;
    }

    result.stdoutStderr = *capturedOpt;

    if (exitCode != 0) {
        result.errors.push_back(
            "VirtualDub exited with code " + std::to_string(exitCode) +
            "; see stdoutStderr for details");
        return result;
    }

    // Verify output AVI was actually created (VP62 codec may not be installed).
    if (!std::filesystem::exists(outputPath)) {
        result.errors.push_back(
            "VirtualDub exited 0 but output AVI was not created: " +
            outputPath.string() +
            "; ensure the On2 VP6 (VP62) VFW codec is installed");
        return result;
    }

    // ── Validate ──────────────────────────────────────────────────────────────
    if (!validateVp6EncoderOutput(outputPath, normalizedInput, result))
        return result;

    result.success = true;
    return result;
}

std::filesystem::path VirtualDubVp6EncoderBackend::findVirtualDub()
{
    // 1. VIRTUALDUB_EXE environment variable — user override takes precedence.
    if (const char* envVal = std::getenv("VIRTUALDUB_EXE")) {
        std::filesystem::path p(envVal);
        if (!p.empty() && std::filesystem::exists(p))
            return p;
    }

#ifdef _WIN32
    // 2. Common Windows installation paths.
    //    Try 64-bit binaries first (VirtualDub 2.x), then 32-bit.
    static const char* const kCandidates[] = {
        "C:\\Program Files\\VirtualDub2\\VirtualDub64.exe",
        "C:\\Program Files\\VirtualDub2\\VirtualDub.exe",
        "C:\\Program Files\\VirtualDub\\VirtualDub64.exe",
        "C:\\Program Files\\VirtualDub\\VirtualDub.exe",
        "C:\\Program Files (x86)\\VirtualDub\\VirtualDub.exe",
        "C:\\Program Files (x86)\\VirtualDub2\\VirtualDub.exe",
        "C:\\Tools\\VirtualDub\\VirtualDub64.exe",
        "C:\\Tools\\VirtualDub\\VirtualDub.exe",
        nullptr
    };

    for (const char* const* c = kCandidates; *c; ++c) {
        std::filesystem::path candidate(*c);
        if (std::filesystem::exists(candidate))
            return candidate;
    }
#endif

    return {};  // not found
}

// ─────────────────────────────────────────────────────────────────────────────
// validateVp6EncoderOutput
// ─────────────────────────────────────────────────────────────────────────────

bool validateVp6EncoderOutput(const std::filesystem::path& encodedPath,
                               const NormalizedVideoInfo&   normalizedInput,
                               Vp6EncodingResult&           result)
{
    std::string probeErr;
    auto probeOpt = probeSourceVideoWithFfmpeg(encodedPath, probeErr);

    if (!probeOpt) {
        result.errors.push_back(
            "Could not probe encoded output file: " + probeErr);
        return false;
    }

    const VideoSourceInfo& probe = *probeOpt;

    if (!probe.valid || !probe.hasVideo) {
        result.errors.push_back(
            "Encoded output has no valid video stream");
        return false;
    }

    // Populate result fields from probe.
    result.width  = probe.width;
    result.height = probe.height;
    result.fps    = probe.fps;

    if (probe.videoFrameCountEstimate > 0) {
        const std::uint64_t clamped =
            std::min(probe.videoFrameCountEstimate,
                     static_cast<std::uint64_t>(
                         std::numeric_limits<std::uint32_t>::max()));
        result.frameCount = static_cast<std::uint32_t>(clamped);
    } else if (probe.fps > 0.0 && probe.durationSec > 0.0) {
        result.frameCount = static_cast<std::uint32_t>(
            std::round(probe.durationSec * probe.fps));
    }

    bool allValid = true;

    // 1. Codec must be VP6.
    {
        std::string codecLower = probe.videoCodec;
        std::transform(codecLower.begin(), codecLower.end(), codecLower.begin(),
            [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (codecLower.find("vp6") == std::string::npos) {
            result.errors.push_back(
                "Encoded output codec '" + probe.videoCodec + "' is not VP6; "
                "FFmpeg may have substituted a different codec");
            allValid = false;
        }
    }

    // 2. Dimensions must match normalized input exactly.
    if (normalizedInput.width > 0 && normalizedInput.height > 0) {
        if (result.width  != normalizedInput.width ||
            result.height != normalizedInput.height)
        {
            result.errors.push_back(
                "Encoded dimensions " +
                std::to_string(result.width) + "x" +
                std::to_string(result.height) +
                " do not match normalized input " +
                std::to_string(normalizedInput.width) + "x" +
                std::to_string(normalizedInput.height));
            allValid = false;
        }
    }

    // 3. FPS must match within tolerance.
    if (normalizedInput.fps > 0.0 && result.fps > 0.0) {
        const double diff = std::abs(result.fps - normalizedInput.fps);
        if (diff > normalizedInput.fps * kFpsEpsilonFrac) {
            result.errors.push_back(
                "Encoded fps " + fmtFps(result.fps) +
                " does not match normalized input " + fmtFps(normalizedInput.fps));
            allValid = false;
        }
    } else if (normalizedInput.fps > 0.0 && result.fps <= 0.0) {
        result.warnings.push_back(
            "Could not determine fps from encoded output probe; "
            "fps validation skipped");
    }

    // 4. Frame count must be non-zero.
    if (result.frameCount == 0) {
        result.errors.push_back(
            "Encoded output has zero detected frames");
        allValid = false;
    }

    result.valid = allValid;
    return allValid;
}

// ─────────────────────────────────────────────────────────────────────────────
// selectVp6EncoderBackend
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<IVp6EncoderBackend>
selectVp6EncoderBackend(Vp6EncoderSelectionResult& selectionOut)
{
    selectionOut = {};

    // ── 1. Try VirtualDub + On2 VP62 VFW codec (preferred) ───────────────────
    // This is the community-proven path for EA Sports game video replacement.
    // FFmpeg's vp6f encoder is absent from most distribution builds.
    {
        std::filesystem::path vdPath = VirtualDubVp6EncoderBackend::findVirtualDub();
        if (!vdPath.empty()) {
            auto backend = std::make_unique<VirtualDubVp6EncoderBackend>(vdPath);
            if (backend->available()) {
                selectionOut.selectedKind = Vp6EncoderBackendKind::VirtualDub;
                selectionOut.selectedName = backend->displayName();
                return backend;
            }
        } else {
            selectionOut.warnings.push_back(
                "VirtualDub not found; checked common installation paths and "
                "VIRTUALDUB_EXE environment variable. "
                "Install VirtualDub with the On2 VP6 (VP62) VFW codec for best results.");
        }
    }

    // ── 2. Try FFmpeg VP6 encoder ─────────────────────────────────────────────
    // Rarely available — most FFmpeg distribution builds omit the vp6f encoder.
    Vp6EncoderProbeResult ffmpegProbe = probeVp6EncoderSupport();

    if (ffmpegProbe.support == Vp6EncoderSupport::Available &&
        !ffmpegProbe.encoderName.empty())
    {
        selectionOut.selectedKind = Vp6EncoderBackendKind::Ffmpeg;
        selectionOut.selectedName = "FFmpeg (" + ffmpegProbe.encoderName + ")";
        return std::make_unique<FfmpegVp6EncoderBackend>(std::move(ffmpegProbe));
    }

    if (ffmpegProbe.support == Vp6EncoderSupport::NotAvailable) {
        selectionOut.warnings.push_back(
            "FFmpeg is available but its build includes no VP6 video encoder "
            "(vp6f / vp6); most distribution builds omit this encoder");
    } else {
        selectionOut.warnings.push_back(
            "FFmpeg could not be probed for VP6 encoder support; "
            "is it installed and on PATH?");
    }

    // ── 3. External tool seam (not yet wired to any config source) ────────────
    selectionOut.warnings.push_back(
        "No external VP6 encoder tool is configured");

    // ── 4. No backend available ───────────────────────────────────────────────
    selectionOut.selectedKind = Vp6EncoderBackendKind::None;
    selectionOut.selectedName = "(none)";
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// runVp6EncodingStage
// ─────────────────────────────────────────────────────────────────────────────

Vp6EncodingStageResult
runVp6EncodingStage(const NormalizedVideoInfo&   normalizedInput,
                    const std::filesystem::path& workingDirectory,
                    std::string&                 errorOut)
{
    Vp6EncodingStageResult result;
    result.normalizedInput = normalizedInput;

    if (!normalizedInput.valid) {
        errorOut = "runVp6EncodingStage: normalizedInput is not valid";
        result.blockingReasons.push_back(
            "Normalized input is not valid; cannot proceed to encoding");
        return result;
    }

    // ── 1. Select backend ─────────────────────────────────────────────────────
    auto backend = selectVp6EncoderBackend(result.selection);

    for (const auto& w : result.selection.warnings)
        result.warnings.push_back("[selection] " + w);

    if (!backend || !backend->available()) {
        result.blockingReasons.push_back(
            "No usable VP6 encoder backend is configured or available "
            "(run 'ffmpeg -encoders | grep vp6' to check FFmpeg VP6 support)");
        return result;
    }

    // ── 2. Execute encoding ───────────────────────────────────────────────────
    result.encodeResult = backend->encode(normalizedInput, workingDirectory);

    for (const auto& w : result.encodeResult.warnings)
        result.warnings.push_back("[encoding] " + w);

    if (!result.encodeResult.success) {
        for (const auto& e : result.encodeResult.errors)
            result.blockingReasons.push_back("[encoding] " + e);
        return result;
    }

    // ── 3. readyForContainerization ───────────────────────────────────────────
    // Require both success (FFmpeg exited 0) and valid (codec/dims/fps/frames ok).
    result.readyForContainerization = result.encodeResult.valid;

    if (!result.readyForContainerization) {
        for (const auto& e : result.encodeResult.errors)
            result.blockingReasons.push_back("[validation] " + e);
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostics
// ─────────────────────────────────────────────────────────────────────────────

std::string describeVp6EncodingResult(const Vp6EncodingResult& result)
{
    std::ostringstream ss;
    ss << "VP6 Encoding Result\n";

    const char* kindStr = "none";
    switch (result.backendKind) {
    case Vp6EncoderBackendKind::VirtualDub:   kindStr = "VirtualDub"; break;
    case Vp6EncoderBackendKind::Ffmpeg:       kindStr = "FFmpeg"; break;
    case Vp6EncoderBackendKind::ExternalTool: kindStr = "External tool"; break;
    default: break;
    }
    ss << "- Backend: " << kindStr << "\n";
    ss << "- Success: " << (result.success ? "yes" : "no") << "\n";

    if (!result.outputPath.empty())
        ss << "- Output: " << result.outputPath.string() << "\n";

    if (!result.commandSummary.empty())
        ss << "- Command: " << result.commandSummary << "\n";

    if (result.valid) {
        ss << "- Width: "       << result.width      << "\n";
        ss << "- Height: "      << result.height     << "\n";
        if (result.fps > 0.0)
            ss << "- FPS: "     << fmtFps(result.fps) << "\n";
        ss << "- Frame count: " << result.frameCount << "\n";
        ss << "- Valid: yes\n";
    }

    if (!result.warnings.empty()) {
        ss << "- Warnings:\n";
        for (const auto& w : result.warnings)
            ss << "  ! " << w << "\n";
    }
    if (!result.errors.empty()) {
        ss << "- Errors:\n";
        for (const auto& e : result.errors)
            ss << "  * " << e << "\n";
    }

    return ss.str();
}

std::string describeVp6EncodingStageResult(const Vp6EncodingStageResult& result)
{
    std::ostringstream ss;
    ss << "VP6 Encoding Stage\n";
    ss << "- Ready for containerization: "
       << (result.readyForContainerization ? "yes" : "no") << "\n";

    // Backend
    {
        const char* kindStr = "none";
        switch (result.selection.selectedKind) {
        case Vp6EncoderBackendKind::VirtualDub:   kindStr = "VirtualDub"; break;
        case Vp6EncoderBackendKind::Ffmpeg:       kindStr = "FFmpeg"; break;
        case Vp6EncoderBackendKind::ExternalTool: kindStr = "External tool"; break;
        default: break;
        }
        ss << "- Backend: " << kindStr;
        if (result.selection.selectedKind != Vp6EncoderBackendKind::None &&
            !result.selection.selectedName.empty())
        {
            ss << " (" << result.selection.selectedName << ")";
        }
        ss << "\n";
    }

    // Normalized input
    if (result.normalizedInput.valid) {
        const auto& ni = result.normalizedInput;
        ss << "- Input: " << ni.width << "x" << ni.height;
        if (ni.fps > 0.0)
            ss << " @ " << fmtFps(ni.fps) << " fps";
        ss << ", " << ni.frameCount << " frames\n";
    }

    // Encode result
    if (result.encodeResult.success && result.encodeResult.valid) {
        const auto& er = result.encodeResult;
        ss << "- Output: "      << er.outputPath.string() << "\n";
        ss << "- Width: "       << er.width               << "\n";
        ss << "- Height: "      << er.height              << "\n";
        if (er.fps > 0.0)
            ss << "- FPS: "     << fmtFps(er.fps)         << "\n";
        ss << "- Frame count: " << er.frameCount          << "\n";
        ss << "- Valid: yes\n";
    }

    if (!result.warnings.empty()) {
        ss << "- Warnings:\n";
        for (const auto& w : result.warnings)
            ss << "  ! " << w << "\n";
    }
    if (!result.blockingReasons.empty()) {
        ss << "- Blocking reasons:\n";
        for (const auto& r : result.blockingReasons)
            ss << "  * " << r << "\n";
    }

    return ss.str();
}

} // namespace gf::media
