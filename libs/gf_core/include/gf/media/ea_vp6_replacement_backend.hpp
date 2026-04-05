#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// gf::media  EA VP6 replacement backend interface
//
// EaVp6ReplacementBackend is a pure virtual seam for the actual VP6 replacement
// operation.  The GUI layer calls replaceWithMp4() after the user confirms the
// preflight result.
//
// NullEaVp6ReplacementBackend is the default implementation: it reports itself
// as unavailable and refuses to do any work.  Swap in a real implementation
// when the VP6 encoder pipeline is ready.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gf::media {

class EaVp6ReplacementBackend {
public:
    virtual ~EaVp6ReplacementBackend() = default;

    // Returns true when this backend is capable of performing a replacement.
    // The GUI uses this to decide whether the "Proceed" button should be enabled
    // in the preflight dialog.
    virtual bool available() const = 0;

    // Replace the EA VP6 data in originalEaVp6Bytes with re-encoded VP6 data
    // sourced from sourceVideoPath.
    //
    // On success: returns the new EA VP6 byte stream (ready to be written back
    //             to the container).
    // On failure: returns std::nullopt and sets errorOut to a human-readable
    //             reason.
    // Must NOT be called when available() == false.
    virtual std::optional<std::vector<std::uint8_t>>
    replaceWithMp4(const std::vector<std::uint8_t>& originalEaVp6Bytes,
                   const std::filesystem::path&      sourceVideoPath,
                   std::string&                      errorOut) = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Null implementation — always unavailable
// ─────────────────────────────────────────────────────────────────────────────

class NullEaVp6ReplacementBackend final : public EaVp6ReplacementBackend {
public:
    bool available() const override { return false; }

    std::optional<std::vector<std::uint8_t>>
    replaceWithMp4(const std::vector<std::uint8_t>& /*originalEaVp6Bytes*/,
                   const std::filesystem::path&      /*sourceVideoPath*/,
                   std::string&                      errorOut) override
    {
        errorOut = "VP6 replacement backend is not available in this build.";
        return std::nullopt;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Pipeline implementation — VirtualDub → AVI VP6 → EA containerization
// ─────────────────────────────────────────────────────────────────────────────
//
// Pipeline (all steps run synchronously):
//   1. runNormalizationStage()    — FFmpeg CFR + yuv420p intermediate
//   2. runVp6EncodingStage()      — VirtualDub VP62 encode (or FFmpeg fallback)
//   3. containerizeVp6AviToEa()  — AVI demux + EA VP6 synthesis
//      (or containerizeVp6FlvToEa() if FFmpeg backend was used)
//
// available() returns true when at least one VP6 encoder backend is found.
// The temp working directory is created under std::filesystem::temp_directory_path()
// and cleaned up after each call.
//
// Optional progress callback: called with a status string at each pipeline stage.
// The callback is not required; pass nullptr to suppress progress notifications.
class PipelineEaVp6ReplacementBackend final : public EaVp6ReplacementBackend {
public:
    using ProgressCallback = std::function<void(const std::string&)>;

    explicit PipelineEaVp6ReplacementBackend(ProgressCallback progressCb = {});

    // Returns true when at least one VP6 encoder backend (VirtualDub or FFmpeg)
    // is detected.  Does NOT guarantee the On2 VP62 VFW codec is installed
    // (that can only be verified by attempting an actual encode).
    bool available() const override;

    std::optional<std::vector<std::uint8_t>>
    replaceWithMp4(const std::vector<std::uint8_t>& originalEaVp6Bytes,
                   const std::filesystem::path&      sourceVideoPath,
                   std::string&                      errorOut) override;

private:
    ProgressCallback m_progressCb;
    void progress(const std::string& msg) const;
};

} // namespace gf::media
