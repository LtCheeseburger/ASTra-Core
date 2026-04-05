#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// gf::media  VP6 encoder adapter layer
//
// This module sits between the normalization stage and the future EA VP6
// container rebuild.  It does NOT touch AST data.
//
// Pipeline position:
//   normalized intermediate video (yuv420p, CFR, target dims, no audio)
//     → IVp6EncoderBackend::encode()
//     → Vp6EncodingResult  (VP6 bitstream / container on disk)
//     → future: EA VP6 frame-chunk injection
//
// Responsibilities:
//   1. Abstract the encoder behind IVp6EncoderBackend
//   2. Probe + select the best available backend
//   3. Execute encoding and capture output
//   4. Validate the encoded output (codec, dimensions, fps, frame count)
//   5. Report readyForContainerization honestly
//
// Nothing here modifies AST data or performs EA VP6 container rebuild.
// ─────────────────────────────────────────────────────────────────────────────

#include "gf/media/ea_vp6_import_prep.hpp"  // NormalizedVideoInfo, VideoSourceInfo,
                                             // probeVp6EncoderSupport, probeSourceVideoWithFfmpeg

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gf::media {

// ─────────────────────────────────────────────────────────────────────────────
// Encoder backend kind
// ─────────────────────────────────────────────────────────────────────────────

enum class Vp6EncoderBackendKind {
    None,          // no usable encoder available
    VirtualDub,    // VirtualDub + On2 VP6 (VP62) VFW codec  — preferred
    Ffmpeg,        // FFmpeg's vp6f / vp6 encoder              — rarely available
    ExternalTool,  // user-configured external encoder binary
};

// ─────────────────────────────────────────────────────────────────────────────
// What the encoder produced
// ─────────────────────────────────────────────────────────────────────────────

enum class Vp6EncodedOutputKind {
    Unknown,
    RawVp6Bitstream,   // raw VP6 frame data (no container wrapper)
    ContainerizedVp6,  // VP6 inside a container (FLV, AVI, …)
    Other,
};

// ─────────────────────────────────────────────────────────────────────────────
// Structured encode result
// ─────────────────────────────────────────────────────────────────────────────

struct Vp6EncodingResult {
    bool                  success     = false;
    Vp6EncoderBackendKind backendKind = Vp6EncoderBackendKind::None;
    Vp6EncodedOutputKind  outputKind  = Vp6EncodedOutputKind::Unknown;

    std::filesystem::path outputPath;
    std::string           commandSummary;   // human-readable argv (no "2>&1")
    std::string           stdoutStderr;     // captured FFmpeg / tool output

    // Populated by validateVp6EncoderOutput() after successful encode.
    std::uint32_t width      = 0;
    std::uint32_t height     = 0;
    double        fps        = 0.0;
    std::uint32_t frameCount = 0;
    bool          valid      = false;  // true when validation passes

    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

// ─────────────────────────────────────────────────────────────────────────────
// Encoder backend interface
// ─────────────────────────────────────────────────────────────────────────────

class IVp6EncoderBackend {
public:
    virtual ~IVp6EncoderBackend() = default;

    virtual Vp6EncoderBackendKind kind()        const = 0;
    virtual bool                  available()   const = 0;
    virtual std::string           displayName() const = 0;

    // Encode the already-normalized intermediate video to VP6.
    //
    // Preconditions:
    //   - normalizedInput.valid == true
    //   - normalizedInput has correct dimensions, CFR fps, yuv420p, no audio
    //   - workingDirectory exists and is writable
    //
    // The backend chooses an output filename inside workingDirectory.
    // Returns a populated Vp6EncodingResult; success=false on any failure.
    // Does NOT modify AST data.
    virtual Vp6EncodingResult
    encode(const NormalizedVideoInfo&   normalizedInput,
           const std::filesystem::path& workingDirectory) const = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// FFmpeg VP6 backend
// ─────────────────────────────────────────────────────────────────────────────

class FfmpegVp6EncoderBackend final : public IVp6EncoderBackend {
public:
    // Construct with the result of probeVp6EncoderSupport().
    // available() is true only when probe.support == Available AND
    // probe.encoderName is non-empty.
    explicit FfmpegVp6EncoderBackend(Vp6EncoderProbeResult probeResult);

    Vp6EncoderBackendKind kind()        const override;
    bool                  available()   const override;
    std::string           displayName() const override;

    // Runs: ffmpeg -y -i <normalized> -c:v <encoderName> -an <output.flv>
    // Output kind: ContainerizedVp6 (FLV container).
    // Validates the output via probeSourceVideoWithFfmpeg().
    Vp6EncodingResult
    encode(const NormalizedVideoInfo&   normalizedInput,
           const std::filesystem::path& workingDirectory) const override;

private:
    Vp6EncoderProbeResult m_probe;
};

// ─────────────────────────────────────────────────────────────────────────────
// VirtualDub VP6 backend
//
// Uses VirtualDub (1.x or 2.x) with the On2 VP6 (VP62) VFW codec installed.
// This is the community-proven path for EA Sports game video replacement;
// the FFmpeg vp6f encoder is absent from most distribution builds.
//
// Workflow:
//   1. Write a VDScript to workingDirectory.
//   2. Execute: VirtualDub.exe /s <script> /x
//   3. VirtualDub writes VP6-encoded output to <workingDirectory>/vd_encoded.avi
//   4. Validate output via probeSourceVideoWithFfmpeg().
//
// VDScript codec configuration:
//   VirtualDub.video.SetCompression(0x32365056, 0, 0, 8500)
//     handler  = 0x32365056 = MAKEFOURCC('V','P','6','2')
//     lKey     = 0          — keyframe interval: codec default
//     lDataRate= 0          — quality mode (not constant bitrate)
//     lQ       = 8500       — 85% quality
//
// Prerequisites:
//   - VirtualDub.exe / VirtualDub64.exe present (see findVirtualDub()).
//   - On2 VP6 (VP62) VFW codec installed on the system.
// ─────────────────────────────────────────────────────────────────────────────

class VirtualDubVp6EncoderBackend final : public IVp6EncoderBackend {
public:
    // Construct with a path to VirtualDub.exe or VirtualDub64.exe.
    // An empty or non-existent path means not available.
    explicit VirtualDubVp6EncoderBackend(std::filesystem::path executablePath = {});

    Vp6EncoderBackendKind kind()        const override;
    bool                  available()   const override;
    std::string           displayName() const override;

    // Writes a VDScript, runs VirtualDub, validates the produced AVI.
    // Output: workingDirectory / "vd_encoded.avi", kind=ContainerizedVp6.
    Vp6EncodingResult
    encode(const NormalizedVideoInfo&   normalizedInput,
           const std::filesystem::path& workingDirectory) const override;

    // Search common VirtualDub installation paths (Windows).
    // Also checks the VIRTUALDUB_EXE environment variable.
    // Returns an empty path when VirtualDub is not found.
    static std::filesystem::path findVirtualDub();

private:
    std::filesystem::path m_executablePath;
};

// ─────────────────────────────────────────────────────────────────────────────
// External-tool backend placeholder
// ─────────────────────────────────────────────────────────────────────────────

class ExternalVp6EncoderBackend final : public IVp6EncoderBackend {
public:
    // toolPath: path to the external VP6 encoder binary.
    // An empty or non-existent path means not available.
    explicit ExternalVp6EncoderBackend(std::filesystem::path toolPath = {});

    Vp6EncoderBackendKind kind()        const override;
    bool                  available()   const override;
    std::string           displayName() const override;

    // Not yet implemented.  Returns a clear "not implemented" error result.
    // Wire in real command construction here when an external tool is supported.
    Vp6EncodingResult
    encode(const NormalizedVideoInfo&   normalizedInput,
           const std::filesystem::path& workingDirectory) const override;

private:
    std::filesystem::path m_toolPath;
};

// ─────────────────────────────────────────────────────────────────────────────
// Backend selection
// ─────────────────────────────────────────────────────────────────────────────

struct Vp6EncoderSelectionResult {
    Vp6EncoderBackendKind selectedKind = Vp6EncoderBackendKind::None;
    std::string           selectedName;
    std::vector<std::string> warnings;
};

// Probe all candidate backends and return the best available one.
//
// Selection order (preferred first):
//   1. VirtualDub — if VirtualDubVp6EncoderBackend::findVirtualDub() succeeds
//   2. FFmpeg     — if probeVp6EncoderSupport() finds a VP6 video encoder
//   3. ExternalTool — if a tool path is configured and exists
//   4. None — no usable encoder found
//
// Returns nullptr when no backend is available (selectedKind == None).
// selectionOut is always populated, even on nullptr return.
std::unique_ptr<IVp6EncoderBackend>
selectVp6EncoderBackend(Vp6EncoderSelectionResult& selectionOut);

// ─────────────────────────────────────────────────────────────────────────────
// Output validation
// ─────────────────────────────────────────────────────────────────────────────

// Probe the encoded output file and validate it against the normalized input.
//
// Checks:
//   - video codec name contains "vp6" (case-insensitive)  — hard error if not
//   - dimensions match normalizedInput exactly              — hard error if not
//   - fps matches normalizedInput within 0.5% tolerance    — hard error if not
//   - frame count > 0                                      — hard error if not
//
// Populates result.width/height/fps/frameCount/valid on success.
// Returns false and appends to result.errors on any failure.
// Backend-agnostic: works for FLV, AVI, or any probeable container.
bool validateVp6EncoderOutput(const std::filesystem::path& encodedPath,
                               const NormalizedVideoInfo&   normalizedInput,
                               Vp6EncodingResult&           result);

// ─────────────────────────────────────────────────────────────────────────────
// Stage orchestration
// ─────────────────────────────────────────────────────────────────────────────

struct Vp6EncodingStageResult {
    // readyForContainerization = true ONLY when:
    //   encodeResult.success == true
    //   encodeResult.valid   == true  (validation passed)
    bool readyForContainerization = false;

    NormalizedVideoInfo       normalizedInput;
    Vp6EncoderSelectionResult selection;
    Vp6EncodingResult         encodeResult;

    std::vector<std::string> blockingReasons;
    std::vector<std::string> warnings;
};

// Run the full VP6 encoding stage:
//   1. Select backend  (probes FFmpeg, checks external tool)
//   2. If no backend:  return readyForContainerization=false with blockingReasons
//   3. Execute encode
//   4. Validate output
//   5. readyForContainerization = encodeResult.success && encodeResult.valid
//
// errorOut is set only on a catastrophic internal failure (e.g. invalid input).
// Normal "encoder not available" is expressed through blockingReasons.
Vp6EncodingStageResult
runVp6EncodingStage(const NormalizedVideoInfo&   normalizedInput,
                    const std::filesystem::path& workingDirectory,
                    std::string&                 errorOut);

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostics
// ─────────────────────────────────────────────────────────────────────────────

std::string describeVp6EncodingResult(const Vp6EncodingResult& result);
std::string describeVp6EncodingStageResult(const Vp6EncodingStageResult& result);

} // namespace gf::media
