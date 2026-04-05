#include "gf/media/ea_vp6_replacement_backend.hpp"
#include "gf/media/ea_vp6_import_prep.hpp"
#include "gf/media/vp6_encoder_backend.hpp"
#include "gf/media/ea_vp6_containerizer.hpp"

#include <chrono>
#include <filesystem>
#include <sstream>
#include <system_error>

namespace gf::media {

// ─────────────────────────────────────────────────────────────────────────────
// Internal: unique temp working directory
// ─────────────────────────────────────────────────────────────────────────────

static std::filesystem::path makeTempWorkDir(std::string& errorOut)
{
    std::error_code ec;
    const auto base = std::filesystem::temp_directory_path(ec);
    if (ec) {
        errorOut = "Could not determine temp directory: " + ec.message();
        return {};
    }

    // Unique suffix from steady clock to avoid collisions.
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path workDir =
        base / ("astra_vp6_" + std::to_string(ticks));

    std::filesystem::create_directories(workDir, ec);
    if (ec) {
        errorOut = "Could not create temp work directory " +
                   workDir.string() + ": " + ec.message();
        return {};
    }
    return workDir;
}

// ─────────────────────────────────────────────────────────────────────────────
// PipelineEaVp6ReplacementBackend
// ─────────────────────────────────────────────────────────────────────────────

PipelineEaVp6ReplacementBackend::PipelineEaVp6ReplacementBackend(ProgressCallback progressCb)
    : m_progressCb(std::move(progressCb))
{}

void PipelineEaVp6ReplacementBackend::progress(const std::string& msg) const
{
    if (m_progressCb) m_progressCb(msg);
}

bool PipelineEaVp6ReplacementBackend::available() const
{
    // VirtualDub (preferred)
    if (!VirtualDubVp6EncoderBackend::findVirtualDub().empty())
        return true;

    // FFmpeg with VP6 encoder (rarely available in distribution builds)
    const Vp6EncoderProbeResult probe = probeVp6EncoderSupport();
    return probe.support == Vp6EncoderSupport::Available &&
           !probe.encoderName.empty();
}

std::optional<std::vector<std::uint8_t>>
PipelineEaVp6ReplacementBackend::replaceWithMp4(
    const std::vector<std::uint8_t>& originalEaVp6Bytes,
    const std::filesystem::path&      sourceVideoPath,
    std::string&                      errorOut)
{
    // ── Create temp working directory ─────────────────────────────────────────
    const std::filesystem::path workDir = makeTempWorkDir(errorOut);
    if (workDir.empty()) return std::nullopt;

    // Cleanup on exit (best-effort — do not fail the operation if cleanup fails).
    struct TempDirGuard {
        std::filesystem::path path;
        ~TempDirGuard() {
            std::error_code ec;
            if (!path.empty()) std::filesystem::remove_all(path, ec);
        }
    } tempGuard{workDir};

    // ── Stage 1: Normalize source video ──────────────────────────────────────
    progress("Normalizing source video (FFmpeg CFR pass)…");

    const std::filesystem::path normalizedPath = workDir / "normalized.mp4";
    std::string normErr;
    const EaVp6NormalizationStageResult normResult =
        runNormalizationStage(originalEaVp6Bytes, sourceVideoPath,
                               normalizedPath, normErr);

    if (!normErr.empty()) {
        errorOut = "Normalization stage internal error: " + normErr;
        return std::nullopt;
    }
    if (!normResult.readyForEncoding) {
        std::ostringstream ss;
        ss << "Normalization failed:";
        for (const auto& r : normResult.blockingReasons)
            ss << "\n  * " << r;
        errorOut = ss.str();
        return std::nullopt;
    }

    // ── Stage 2: Encode to VP6 ────────────────────────────────────────────────
    progress("Encoding to VP6 (VirtualDub + On2 VP62)…");

    std::string encodeErr;
    const Vp6EncodingStageResult encodeResult =
        runVp6EncodingStage(normResult.execution.normalizedInfo,
                             workDir, encodeErr);

    if (!encodeErr.empty()) {
        errorOut = "VP6 encoding stage internal error: " + encodeErr;
        return std::nullopt;
    }
    if (!encodeResult.readyForContainerization) {
        std::ostringstream ss;
        ss << "VP6 encoding failed:";
        for (const auto& r : encodeResult.blockingReasons)
            ss << "\n  * " << r;
        errorOut = ss.str();
        return std::nullopt;
    }

    // ── Stage 3: Containerize into EA VP6 format ──────────────────────────────
    progress("Building EA VP6 container…");

    // Parse the target header from the original bytes to reconstruct MVhd.
    std::string targetErr;
    const auto targetOpt = analyzeEaVp6ImportTarget(
        std::span<const std::uint8_t>(originalEaVp6Bytes.data(),
                                       originalEaVp6Bytes.size()),
        targetErr);
    if (!targetOpt) {
        errorOut = "Could not re-parse original EA VP6 target: " + targetErr;
        return std::nullopt;
    }
    const EaVp6ImportTarget& target = *targetOpt;

    std::optional<std::vector<std::uint8_t>> eaBytesOpt;
    std::string containerErr;

    const Vp6EncoderBackendKind backendKind =
        encodeResult.encodeResult.backendKind;

    if (backendKind == Vp6EncoderBackendKind::VirtualDub) {
        // VirtualDub output is an AVI — use the AVI containerizer path.
        eaBytesOpt = containerizeVp6AviToEa(
            encodeResult.encodeResult.outputPath, target, containerErr);
    } else {
        // FFmpeg output is an FLV — use the FLV containerizer path.
        eaBytesOpt = containerizeVp6FlvToEa(
            encodeResult.encodeResult.outputPath, target, containerErr);
    }

    if (!eaBytesOpt) {
        errorOut = "EA VP6 containerization failed: " + containerErr;
        return std::nullopt;
    }

    progress("Replacement complete.");
    return eaBytesOpt;
}

} // namespace gf::media
