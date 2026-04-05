#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// gf::media  EA VP6 import-preparation and normalization layer
//
// Split into two stages:
//
// STAGE A — Preflight (non-executing, fast):
//   1. Analyse the original EA VP6 target (dimensions, fps, frame counts)
//   2. Probe the user-supplied source video via ffprobe
//   3. Build a normalisation plan (resize, fps-convert, strip audio)
//   4. Probe whether a VP6 encoder is available in the local FFmpeg build
//   5. Assemble a combined result with a clear possible/not-possible verdict
//
// STAGE B — Normalization execution (executes FFmpeg, writes an intermediate):
//   6. Execute the normalization plan via FFmpeg with strict CFR enforcement
//   7. Probe the normalized output and validate dimensions/fps/frame-count/audio
//   8. Report readyForEncoding and frame-count alignment with the original target
//
// Nothing here modifies AST data or performs VP6 encoding.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gf::media {

// ─────────────────────────────────────────────────────────────────────────────
// 1. Original EA VP6 target analysis
// ─────────────────────────────────────────────────────────────────────────────

struct EaVp6ImportTarget {
    std::uint32_t width                = 0;
    std::uint32_t height               = 0;
    double        fps                  = 0.0;
    std::uint32_t frameCount           = 0;
    std::uint32_t largestChunkSize     = 0;
    // MVhd rational fps fields — preserved for faithful MVhd reconstruction.
    // 0 when headerValid is false or the original MVhd had zero values.
    std::uint32_t fpsDenominator       = 0;   // "rate"  field from MVhd (+0x10)
    std::uint32_t fpsNumerator         = 0;   // "scale" field from MVhd (+0x14)
    // Codec identifier from MVhd (+0x00).  0 when unknown.
    // Typically 0x30367076 ('vp60' in little-endian) for EA VP6 files.
    std::uint32_t codecId              = 0;
    std::size_t   keyframeChunkCount   = 0;
    std::size_t   deltaFrameChunkCount = 0;
    bool          headerValid          = false;
    bool          hasFrameChunks       = false;
    std::vector<std::string> warnings;
};

// Parse originalEaVp6Bytes with EaVp6Parser and extract target constraints.
// Returns std::nullopt only if the file is completely unparseable (structural
// failure).  Partial/suspicious data produces populated warnings instead.
std::optional<EaVp6ImportTarget>
analyzeEaVp6ImportTarget(std::span<const std::uint8_t> originalEaVp6Bytes,
                         std::string& errorOut);

// ─────────────────────────────────────────────────────────────────────────────
// 2. Source video analysis (via ffprobe)
// ─────────────────────────────────────────────────────────────────────────────

struct VideoSourceInfo {
    bool          valid                   = false;
    std::string   containerName;
    std::string   videoCodec;
    std::string   audioCodec;
    std::uint32_t width                   = 0;
    std::uint32_t height                  = 0;
    double        fps                     = 0.0;
    double        durationSec             = 0.0;
    std::uint64_t videoFrameCountEstimate = 0;
    bool          hasVideo                = false;
    bool          hasAudio                = false;
    std::vector<std::string> warnings;
};

// Run ffprobe on inputPath and populate VideoSourceInfo.
// On success sets result.valid = true.
// If ffprobe is unavailable or produces unparseable output, sets valid = false
// and errorOut to a human-readable reason; does not throw.
std::optional<VideoSourceInfo>
probeSourceVideoWithFfmpeg(const std::filesystem::path& inputPath,
                           std::string& errorOut);

// ─────────────────────────────────────────────────────────────────────────────
// 3. Normalisation plan
// ─────────────────────────────────────────────────────────────────────────────

struct VideoNormalizationPlan {
    bool          valid                 = false;
    std::uint32_t outputWidth           = 0;
    std::uint32_t outputHeight          = 0;
    double        outputFps             = 0.0;
    bool          stripAudio            = true;  // MVP default: always strip
    bool          resizeRequired        = false;
    bool          fpsConversionRequired = false;
    bool          reencodeRequired      = true;  // always true for VP6 prep
    std::vector<std::string> steps;
    std::vector<std::string> warnings;
};

// Build a plan that maps source -> target dimensions/fps.
// Does NOT execute any FFmpeg commands.
// Returns invalid plan (valid = false) only if both target and source are
// too incomplete to derive meaningful dimensions.
VideoNormalizationPlan
buildNormalizationPlan(const EaVp6ImportTarget& target,
                       const VideoSourceInfo&   source);

// ─────────────────────────────────────────────────────────────────────────────
// 4. VP6 encoder feasibility probe
// ─────────────────────────────────────────────────────────────────────────────

enum class Vp6EncoderSupport {
    Unknown,       // could not determine (ffmpeg missing / parse failure)
    NotAvailable,  // ffmpeg found but no VP6 encoder present
    Available,     // at least one VP6 video encoder confirmed in the build
};

struct Vp6EncoderProbeResult {
    Vp6EncoderSupport        support = Vp6EncoderSupport::Unknown;
    std::string              encoderName;   // e.g. "vp6f" — populated when Available
    std::vector<std::string> rawLines;      // lines from ffmpeg -encoders mentioning VP6
    std::vector<std::string> warnings;
};

// Run "ffmpeg -encoders" and inspect output for VP6 video encoder entries.
// Conservative: only marks Available when a video VP6 encoder is unambiguously
// listed.  Decoding support does NOT imply encoding support.
// Never throws.
Vp6EncoderProbeResult probeVp6EncoderSupport();

// ─────────────────────────────────────────────────────────────────────────────
// 5. Combined import-preparation result
// ─────────────────────────────────────────────────────────────────────────────

struct EaVp6ImportPreparationResult {
    bool                   possible = false;
    EaVp6ImportTarget      target;
    VideoSourceInfo        source;
    VideoNormalizationPlan normalization;
    Vp6EncoderProbeResult  encoderProbe;
    // Human-readable name of the selected encoder backend (e.g. "VirtualDub (VirtualDub64.exe)").
    // Empty when no encoder is available.
    std::string            encoderBackendName;
    std::vector<std::string> blockingReasons;
    std::vector<std::string> warnings;
};

// Run the full preparation pipeline:
//   analyzeEaVp6ImportTarget → probeSourceVideoWithFfmpeg →
//   buildNormalizationPlan   → probeVp6EncoderSupport
// Sets possible = true only when all three prerequisites are satisfied:
//   - original target parsed well enough (headerValid)
//   - source video is valid
//   - VP6 encoder is Available
// If possible = false, blockingReasons lists every specific obstacle.
// errorOut is set only on a catastrophic internal failure; normal infeasibility
// is expressed through possible + blockingReasons.
EaVp6ImportPreparationResult
prepareEaVp6Import(const std::vector<std::uint8_t>& originalEaVp6Bytes,
                   const std::filesystem::path&     sourceVideoPath,
                   std::string&                     errorOut);

// ─────────────────────────────────────────────────────────────────────────────
// 6. Normalisation command builder  (non-executing)
// ─────────────────────────────────────────────────────────────────────────────

// Build the argv list for an FFmpeg normalization pass.
//
// The command enforces strict CFR output:
//   ffmpeg -y -i <input>
//          [-vf "scale=W:H[,fps=F]"]    scale: only if resizeRequired
//                                        fps: ALWAYS if outputFps > 0
//          -pix_fmt yuv420p
//          -an                           always (audio stripped)
//          -vsync cfr                    CFR enforcement — required
//          <output>
//
// The fps vf filter is always applied when outputFps > 0, regardless of whether
// fpsConversionRequired is set, to guarantee a constant frame rate in the output.
//
// Fails if plan.valid is false or dimensions are zero.
// The returned vector begins with "ffmpeg" and ends with outputPath.string().
std::optional<std::vector<std::string>>
buildNormalizationCommand(const VideoNormalizationPlan& plan,
                          const std::filesystem::path&  inputPath,
                          const std::filesystem::path&  outputPath,
                          std::string&                  errorOut);

// ─────────────────────────────────────────────────────────────────────────────
// 7. Diagnostic summary (Stage A)
// ─────────────────────────────────────────────────────────────────────────────

std::string describeImportPreparation(const EaVp6ImportPreparationResult& result);

// ─────────────────────────────────────────────────────────────────────────────
// Stage B — Normalization execution
// ─────────────────────────────────────────────────────────────────────────────

// ── 8. Normalized video info (post-execution probe result) ────────────────────

enum class IntermediateVideoKind {
    Unknown,
    NormalizedMp4,   // yuv420p, CFR, no audio — ready for VP6 encoding
};

struct NormalizedVideoInfo {
    bool                  valid        = false;
    IntermediateVideoKind kind         = IntermediateVideoKind::Unknown;
    std::filesystem::path filePath;

    std::uint32_t width       = 0;
    std::uint32_t height      = 0;
    double        fps         = 0.0;
    std::uint32_t frameCount  = 0;  // from nb_frames, or round(duration × fps)
    double        durationSec = 0.0;
    bool          hasAudio    = false;

    std::vector<std::string> warnings;
};

// ── 9. Execution result ────────────────────────────────────────────────────────

struct VideoNormalizationExecutionResult {
    bool success = false;

    VideoNormalizationPlan plan;
    std::filesystem::path  outputPath;

    NormalizedVideoInfo normalizedInfo;

    // true when normalizedInfo.frameCount == target.frameCount exactly.
    // A mismatch is a warning, not a failure (source duration may differ).
    bool frameCountMatchesTarget = false;

    std::string ffmpegCommandSummary;  // space-joined argv (for logging/display)
    std::string ffmpegStdoutStderr;    // captured FFmpeg output

    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

// Execute the normalization plan:
//   1. Constructs the FFmpeg argv via buildNormalizationCommand()
//   2. Runs FFmpeg; captures stdout+stderr
//   3. Probes the output file via ffprobe
//   4. Validates dimensions, fps, frame count, and audio against target
//
// On failure (FFmpeg exits non-zero, or validation fails):
//   success = false, errors describes every failure.
// Frame-count mismatch vs target is a warning, not an error.
// Does NOT throw.
VideoNormalizationExecutionResult
executeNormalization(const VideoNormalizationPlan& plan,
                     const EaVp6ImportTarget&      target,
                     const std::filesystem::path&  inputPath,
                     const std::filesystem::path&  outputPath);

// ── 10. Full normalization stage result ────────────────────────────────────────

struct EaVp6NormalizationStageResult {
    // readyForEncoding = true only when:
    //   execution.success == true
    //   normalizedInfo dimensions match target exactly
    //   normalizedInfo fps matches target within tolerance
    //   normalizedInfo.frameCount > 0
    // VP6 encoder availability is NOT checked here.
    bool readyForEncoding = false;

    EaVp6ImportTarget                target;
    VideoSourceInfo                  source;
    VideoNormalizationPlan           plan;
    VideoNormalizationExecutionResult execution;

    std::vector<std::string> blockingReasons;
    std::vector<std::string> warnings;
};

// Run the full normalization pipeline:
//   analyzeEaVp6ImportTarget → probeSourceVideoWithFfmpeg →
//   buildNormalizationPlan   → executeNormalization
//
// errorOut is set only on a catastrophic internal failure (e.g. can't parse the
// original EA VP6 file).  Normal execution failures are expressed through
// readyForEncoding + blockingReasons.
EaVp6NormalizationStageResult
runNormalizationStage(const std::vector<std::uint8_t>& originalEaVp6Bytes,
                      const std::filesystem::path&     sourceVideoPath,
                      const std::filesystem::path&     outputVideoPath,
                      std::string&                     errorOut);

// ── 11. Diagnostic summary (Stage B) ─────────────────────────────────────────

std::string describeNormalizationStage(const EaVp6NormalizationStageResult& result);

} // namespace gf::media
