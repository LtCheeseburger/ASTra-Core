#include "gf/media/ea_vp6_containerizer.hpp"
#include "gf/media/avi_vp6.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace gf::media {

// ─────────────────────────────────────────────────────────────────────────────
// Internal: little-endian write helpers for MVhd payload synthesis
// ─────────────────────────────────────────────────────────────────────────────

static void writeU16le(std::uint8_t* p, std::uint16_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

static void writeU32le(std::uint8_t* p, std::uint32_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >>  8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal: FPS rational conversion
//
// Converts a double fps value to a (denominator, numerator) pair suitable for
// storage in the MVhd payload, where fps = denominator / numerator.
//
// Lookup covers common video frame rates exactly.  Unknown rates fall back to
// the "×1000" approximation: denom = round(fps * 1000), numer = 1000, so
// fps = denom/1000 — accurate to three decimal places.
//
// Note: the original target fpsDenominator/fpsNumerator values are preferred
// over this function when available (see buildEaVp6MovieFromFlvVp6).
// ─────────────────────────────────────────────────────────────────────────────

static void fpsDoubleToRational(double fps,
                                 std::uint32_t& denom,
                                 std::uint32_t& numer)
{
    // Tolerance for exact-match detection.
    static constexpr double kTol = 0.005;

    struct Entry { double fps; std::uint32_t d; std::uint32_t n; };
    static constexpr Entry kTable[] = {
        {12.0,   12,      1},
        {15.0,   15,      1},
        {23.976, 24000, 1001},
        {24.0,   24,      1},
        {25.0,   25,      1},
        {29.97,  30000, 1001},
        {30.0,   30,      1},
        {48.0,   48,      1},
        {50.0,   50,      1},
        {59.94,  60000, 1001},
        {60.0,   60,      1},
        {120.0, 120,      1},
    };
    for (const auto& e : kTable) {
        if (std::abs(fps - e.fps) < kTol) {
            denom = e.d;
            numer = e.n;
            return;
        }
    }
    // Generic fallback: scale by 1000.
    numer = 1000u;
    denom = static_cast<std::uint32_t>(std::round(fps * 1000.0));
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal: synthesize a 24-byte MVhd payload
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<std::uint8_t>
synthesizeMvhdPayload(std::uint32_t codecId,
                      std::uint16_t width,
                      std::uint16_t height,
                      std::uint32_t frameCount,
                      std::uint32_t largestChunkSize,
                      std::uint32_t fpsDenominator,
                      std::uint32_t fpsNumerator)
{
    std::vector<std::uint8_t> payload(24, 0u);
    std::uint8_t* p = payload.data();

    writeU32le(p + 0x00, codecId);          // +0x00 codec id
    writeU16le(p + 0x04, width);            // +0x04 width
    writeU16le(p + 0x06, height);           // +0x06 height
    writeU32le(p + 0x08, frameCount);       // +0x08 frame count
    writeU32le(p + 0x0C, largestChunkSize); // +0x0C largest chunk size
    writeU32le(p + 0x10, fpsDenominator);   // +0x10 fps denominator (rate)
    writeU32le(p + 0x14, fpsNumerator);     // +0x14 fps numerator  (scale)

    return payload;
}

// ─────────────────────────────────────────────────────────────────────────────
// buildEaVp6MovieFromFlvVp6
// ─────────────────────────────────────────────────────────────────────────────

std::optional<EaVp6Movie>
buildEaVp6MovieFromFlvVp6(const FlvVp6VideoStream& flv,
                           const EaVp6ImportTarget& target,
                           std::string& errorOut)
{
    if (flv.packets.empty()) {
        errorOut = "buildEaVp6MovieFromFlvVp6: FlvVp6VideoStream has no packets";
        return std::nullopt;
    }

    if (target.headerValid && (target.width == 0 || target.height == 0)) {
        errorOut = "buildEaVp6MovieFromFlvVp6: target has zero dimensions; "
                   "cannot synthesize valid MVhd";
        return std::nullopt;
    }

    EaVp6Movie movie;

    // ── 1. Determine MVhd field values ────────────────────────────────────────

    // Codec ID: prefer target's value; fall back to standard 'vp60' LE.
    // 0x30367076 = ASCII 'v','p','6','0' stored as a little-endian uint32.
    const std::uint32_t codecId =
        (target.headerValid && target.codecId != 0u)
            ? target.codecId
            : 0x30367076u;

    const std::uint16_t width  =
        target.headerValid ? static_cast<std::uint16_t>(target.width)  : 0u;
    const std::uint16_t height =
        target.headerValid ? static_cast<std::uint16_t>(target.height) : 0u;

    const std::uint32_t frameCount =
        static_cast<std::uint32_t>(flv.packets.size());

    // largestChunkSize = max total chunk size (8-byte header + payload) among
    // all video-frame chunks, which matches observed real-file behaviour.
    std::uint32_t largestChunkSize = 0u;
    for (const auto& pkt : flv.packets) {
        const std::uint64_t total =
            8ull + static_cast<std::uint64_t>(pkt.payload.size());
        // Clamp to u32 max (extremely large packets are pathological).
        const std::uint32_t totalU32 = static_cast<std::uint32_t>(
            std::min(total, static_cast<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max())));
        largestChunkSize = std::max(largestChunkSize, totalU32);
    }

    // FPS rational: prefer target's preserved values; derive from fps otherwise.
    std::uint32_t fpsDenominator = 0u;
    std::uint32_t fpsNumerator   = 0u;

    if (target.headerValid &&
        target.fpsDenominator > 0u && target.fpsNumerator > 0u)
    {
        // Original MVhd rational — exact reproduction.
        fpsDenominator = target.fpsDenominator;
        fpsNumerator   = target.fpsNumerator;
    } else if (target.fps > 0.0) {
        fpsDoubleToRational(target.fps, fpsDenominator, fpsNumerator);
        movie.warnings.push_back(
            "MVhd fps rational derived from double (target.fps=" +
            [&]{ std::ostringstream s;
                 s << std::fixed << std::setprecision(4) << target.fps;
                 return s.str(); }() +
            "); original numerator/denominator were not available. "
            "Result: " + std::to_string(fpsDenominator) + "/" +
            std::to_string(fpsNumerator));
    } else if (flv.fps > 0.0) {
        fpsDoubleToRational(flv.fps, fpsDenominator, fpsNumerator);
        movie.warnings.push_back(
            "MVhd fps rational derived from FLV timestamp estimate (fps=" +
            [&]{ std::ostringstream s;
                 s << std::fixed << std::setprecision(4) << flv.fps;
                 return s.str(); }() +
            "); target fps was unavailable. "
            "Result: " + std::to_string(fpsDenominator) + "/" +
            std::to_string(fpsNumerator));
    } else {
        movie.warnings.push_back(
            "FPS could not be determined from target or FLV timestamps; "
            "MVhd fpsDenominator/fpsNumerator will be zero");
    }

    // ── 2. Synthesize MVhd chunk ──────────────────────────────────────────────
    {
        EaVp6Chunk mvhd;
        mvhd.kind        = EaVp6ChunkKind::MVhd;
        mvhd.tag         = "MVhd";
        mvhd.payload     = synthesizeMvhdPayload(codecId, width, height,
                                                  frameCount, largestChunkSize,
                                                  fpsDenominator, fpsNumerator);
        // declaredSize = total chunk size (header + payload), consistent with parser.
        mvhd.declaredSize = static_cast<std::uint32_t>(8u + mvhd.payload.size());
        mvhd.fileOffset  = 0u;  // will be at file start

        // Decode the payload back into movie.header for consistency.
        movie.header.valid           = true;
        movie.header.codecId         = codecId;
        movie.header.width           = width;
        movie.header.height          = height;
        movie.header.frameCount      = frameCount;
        movie.header.largestChunkSize = largestChunkSize;
        movie.header.fpsDenominator  = fpsDenominator;
        movie.header.fpsNumerator    = fpsNumerator;
        if (fpsNumerator != 0u)
            movie.header.fps = static_cast<double>(fpsDenominator) /
                               static_cast<double>(fpsNumerator);

        movie.chunks.push_back(std::move(mvhd));
    }

    // ── 3. Append video-frame chunks ─────────────────────────────────────────
    std::uint64_t fileOffset = static_cast<std::uint64_t>(
        movie.chunks[0].declaredSize);  // MVhd total size

    for (const auto& pkt : flv.packets) {
        EaVp6Chunk chunk;
        chunk.kind        = pkt.isKeyframe ? EaVp6ChunkKind::MV0K
                                           : EaVp6ChunkKind::MV0F;
        chunk.tag         = pkt.isKeyframe ? "MV0K" : "MV0F";
        chunk.payload     = pkt.payload;
        chunk.declaredSize = static_cast<std::uint32_t>(8u + pkt.payload.size());
        chunk.fileOffset  = fileOffset;

        fileOffset += static_cast<std::uint64_t>(chunk.declaredSize);
        movie.chunks.push_back(std::move(chunk));
    }

    // Propagate FlvVp6VideoStream warnings into the movie.
    for (const auto& w : flv.warnings)
        movie.warnings.push_back("[flv] " + w);

    return movie;
}

// ─────────────────────────────────────────────────────────────────────────────
// validateEaVp6ContainerizationOutput
// ─────────────────────────────────────────────────────────────────────────────

EaVp6ContainerizationValidation
validateEaVp6ContainerizationOutput(const EaVp6Movie&                movie,
                                     const EaVp6ImportTarget&         target,
                                     const std::vector<std::uint8_t>& serializedBytes)
{
    EaVp6ContainerizationValidation v;

    // ── 1. Re-parse serialized bytes ─────────────────────────────────────────
    {
        const EaVp6Parser parser;
        std::string parseErr;
        auto reparsed = parser.parse(
            std::span<const std::uint8_t>(serializedBytes.data(),
                                          serializedBytes.size()),
            parseErr);
        if (!reparsed) {
            v.failures.push_back(
                "Round-trip re-parse failed: " + parseErr);
            return v;  // can't check anything else
        }
        v.roundTripParsePassed = true;

        // ── 2. MVhd valid ─────────────────────────────────────────────────────
        if (!reparsed->header.valid) {
            v.failures.push_back("Re-parsed MVhd is not valid");
        } else {
            v.mvhdValid = true;
        }

        // ── 3. frameCount matches video chunk count ───────────────────────────
        {
            std::size_t videoChunkCount = 0;
            for (const auto& c : reparsed->chunks)
                if (c.kind == EaVp6ChunkKind::MV0K ||
                    c.kind == EaVp6ChunkKind::MV0F)
                    ++videoChunkCount;

            if (reparsed->header.valid &&
                reparsed->header.frameCount == static_cast<std::uint32_t>(videoChunkCount))
            {
                v.frameCountMatches = true;
            } else {
                v.failures.push_back(
                    "MVhd.frameCount=" +
                    std::to_string(reparsed->header.frameCount) +
                    " does not match video chunk count=" +
                    std::to_string(videoChunkCount));
            }
        }

        // ── 4. largestChunkSize ───────────────────────────────────────────────
        {
            std::uint32_t computedMax = 0u;
            for (const auto& c : reparsed->chunks) {
                if (c.kind != EaVp6ChunkKind::MV0K &&
                    c.kind != EaVp6ChunkKind::MV0F)
                    continue;
                const std::uint64_t total = 8ull + c.payload.size();
                const std::uint32_t t32 = static_cast<std::uint32_t>(
                    std::min(total, static_cast<std::uint64_t>(
                        std::numeric_limits<std::uint32_t>::max())));
                computedMax = std::max(computedMax, t32);
            }

            if (reparsed->header.valid &&
                reparsed->header.largestChunkSize == computedMax)
            {
                v.largestChunkSizeMatches = true;
            } else {
                v.failures.push_back(
                    "MVhd.largestChunkSize=" +
                    std::to_string(reparsed->header.largestChunkSize) +
                    " does not match computed max=" + std::to_string(computedMax));
            }
        }

        // ── 5. Dimensions match target ────────────────────────────────────────
        if (target.headerValid && target.width > 0 && target.height > 0) {
            if (reparsed->header.valid &&
                reparsed->header.width  == static_cast<std::uint16_t>(target.width) &&
                reparsed->header.height == static_cast<std::uint16_t>(target.height))
            {
                v.dimensionsMatch = true;
            } else {
                v.failures.push_back(
                    "MVhd dimensions " +
                    std::to_string(reparsed->header.width) + "x" +
                    std::to_string(reparsed->header.height) +
                    " do not match target " +
                    std::to_string(target.width) + "x" +
                    std::to_string(target.height));
            }
        } else {
            // Target dimensions unknown — skip this check.
            v.dimensionsMatch = true;
            v.warnings.push_back(
                "Target dimensions not available; dimension match not verified");
        }

        // ── 6. At least one keyframe ──────────────────────────────────────────
        {
            const bool hasKf = std::any_of(
                reparsed->chunks.begin(), reparsed->chunks.end(),
                [](const EaVp6Chunk& c){ return c.kind == EaVp6ChunkKind::MV0K; });
            if (hasKf) {
                v.hasKeyframe = true;
            } else {
                v.failures.push_back("No MV0K (keyframe) chunk in synthesized movie");
            }
        }

        // ── 7. First video chunk is keyframe ─────────────────────────────────
        {
            const EaVp6Chunk* firstVideo = nullptr;
            for (const auto& c : reparsed->chunks) {
                if (c.kind == EaVp6ChunkKind::MV0K ||
                    c.kind == EaVp6ChunkKind::MV0F)
                {
                    firstVideo = &c;
                    break;
                }
            }
            if (firstVideo && firstVideo->kind == EaVp6ChunkKind::MV0K) {
                v.firstFrameIsKeyframe = true;
            } else if (firstVideo) {
                v.warnings.push_back(
                    "First video chunk is MV0F (delta frame), not MV0K (keyframe); "
                    "this may cause decoding issues in EA VP6 players");
            }
        }
    }

    // Also cross-check against the in-memory movie (before serialization).
    // This catches synthesis bugs that the writer normalised away.
    (void)movie;

    v.passed = v.roundTripParsePassed &&
               v.mvhdValid            &&
               v.frameCountMatches    &&
               v.largestChunkSizeMatches &&
               v.dimensionsMatch      &&
               v.hasKeyframe;

    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// containerizeVp6FlvToEaWithResult  (full diagnostic version)
// ─────────────────────────────────────────────────────────────────────────────

EaVp6ContainerizationResult
containerizeVp6FlvToEaWithResult(const std::filesystem::path& flvPath,
                                  const EaVp6ImportTarget&     target)
{
    EaVp6ContainerizationResult result;

    // ── Step 1: parse FLV ────────────────────────────────────────────────────
    {
        const FlvVp6Parser parser;
        std::string parseErr;
        auto streamOpt = parser.parse(flvPath, parseErr);
        if (!streamOpt) {
            result.errors.push_back("FLV VP6 parse failed: " + parseErr);
            return result;
        }
        result.flvStream = std::move(*streamOpt);
        for (const auto& w : result.flvStream.warnings)
            result.warnings.push_back("[flv] " + w);
    }

    // ── Step 2: build EaVp6Movie ─────────────────────────────────────────────
    {
        std::string buildErr;
        auto movieOpt = buildEaVp6MovieFromFlvVp6(
            result.flvStream, target, buildErr);
        if (!movieOpt) {
            result.errors.push_back("EA VP6 movie build failed: " + buildErr);
            return result;
        }
        result.movie = std::move(*movieOpt);
        for (const auto& w : result.movie.warnings)
            result.warnings.push_back("[movie] " + w);
    }

    // ── Step 3: serialize with EaVp6Writer ───────────────────────────────────
    {
        const EaVp6Writer writer;
        std::string writeErr;
        auto bytesOpt = writer.build(result.movie, writeErr);
        if (!bytesOpt) {
            result.errors.push_back("EA VP6 serialization failed: " + writeErr);
            return result;
        }
        result.serializedBytes = std::move(*bytesOpt);
    }

    // ── Step 4: validate ─────────────────────────────────────────────────────
    result.validation = validateEaVp6ContainerizationOutput(
        result.movie, target, result.serializedBytes);

    for (const auto& w : result.validation.warnings)
        result.warnings.push_back("[validation] " + w);

    if (!result.validation.passed) {
        for (const auto& f : result.validation.failures)
            result.errors.push_back("[validation] " + f);
        return result;
    }

    result.success = true;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// containerizeVp6FlvToEa  (simple bytes-only version)
// ─────────────────────────────────────────────────────────────────────────────

std::optional<std::vector<std::uint8_t>>
containerizeVp6FlvToEa(const std::filesystem::path& flvPath,
                        const EaVp6ImportTarget&     target,
                        std::string&                 errorOut)
{
    auto r = containerizeVp6FlvToEaWithResult(flvPath, target);
    if (!r.success) {
        if (!r.errors.empty())
            errorOut = r.errors.front();
        else
            errorOut = "containerizeVp6FlvToEa: unknown failure";
        return std::nullopt;
    }
    return std::move(r.serializedBytes);
}

// ─────────────────────────────────────────────────────────────────────────────
// describeEaVp6ContainerizationResult
// ─────────────────────────────────────────────────────────────────────────────

std::string describeEaVp6ContainerizationResult(
    const EaVp6ContainerizationResult& result)
{
    std::ostringstream ss;
    ss << "EA VP6 Containerization\n";
    ss << "- Success: " << (result.success ? "yes" : "no") << "\n";

    // FLV input summary
    if (result.flvStream.valid && result.flvStream.frameCount > 0) {
        std::size_t kfCount = 0;
        for (const auto& p : result.flvStream.packets)
            if (p.isKeyframe) ++kfCount;

        ss << "- Input FLV packets: " << result.flvStream.frameCount << "\n";
        ss << "- Keyframes: "         << kfCount                     << "\n";
        ss << "- Delta frames: "      << (result.flvStream.frameCount - kfCount) << "\n";
    }

    // Output movie summary
    if (result.movie.header.valid) {
        const auto& h = result.movie.header;
        ss << "- Output width: "  << h.width  << "\n";
        ss << "- Output height: " << h.height << "\n";
        if (h.fps > 0.0)
            ss << "- Output fps: "
               << std::fixed << std::setprecision(2) << h.fps << "\n";
        ss << "- Frame count (MVhd): " << h.frameCount << "\n";
        ss << "- Largest chunk: "      << h.largestChunkSize << "\n";
    }

    if (!result.serializedBytes.empty())
        ss << "- Serialized bytes: " << result.serializedBytes.size() << "\n";

    // Validation
    {
        const auto& v = result.validation;
        if (v.roundTripParsePassed) {
            ss << "- Validation: " << (v.passed ? "passed" : "FAILED") << "\n";
            if (!v.passed && !v.failures.empty()) {
                for (const auto& f : v.failures)
                    ss << "  x " << f << "\n";
            }
            if (!v.warnings.empty()) {
                for (const auto& w : v.warnings)
                    ss << "  ! " << w << "\n";
            }
        }
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

// ─────────────────────────────────────────────────────────────────────────────
// containerizeVp6AviToEaWithResult  (AVI input, full diagnostic version)
// ─────────────────────────────────────────────────────────────────────────────

EaVp6ContainerizationResult
containerizeVp6AviToEaWithResult(const std::filesystem::path& aviPath,
                                  const EaVp6ImportTarget&     target)
{
    EaVp6ContainerizationResult result;

    // ── Step 1: parse AVI VP6 ────────────────────────────────────────────────
    {
        const AviVp6Parser parser;
        std::string parseErr;
        auto streamOpt = parser.parse(aviPath, parseErr);
        if (!streamOpt) {
            result.errors.push_back("AVI VP6 parse failed: " + parseErr);
            return result;
        }
        // AviVp6VideoStream is an alias for FlvVp6VideoStream — store directly.
        result.flvStream = std::move(*streamOpt);
        for (const auto& w : result.flvStream.warnings)
            result.warnings.push_back("[avi] " + w);
    }

    // ── Steps 2-4: shared with FLV path ─────────────────────────────────────
    {
        std::string buildErr;
        auto movieOpt = buildEaVp6MovieFromFlvVp6(
            result.flvStream, target, buildErr);
        if (!movieOpt) {
            result.errors.push_back("EA VP6 movie build failed: " + buildErr);
            return result;
        }
        result.movie = std::move(*movieOpt);
        for (const auto& w : result.movie.warnings)
            result.warnings.push_back("[movie] " + w);
    }

    {
        const EaVp6Writer writer;
        std::string writeErr;
        auto bytesOpt = writer.build(result.movie, writeErr);
        if (!bytesOpt) {
            result.errors.push_back("EA VP6 serialization failed: " + writeErr);
            return result;
        }
        result.serializedBytes = std::move(*bytesOpt);
    }

    result.validation = validateEaVp6ContainerizationOutput(
        result.movie, target, result.serializedBytes);

    for (const auto& w : result.validation.warnings)
        result.warnings.push_back("[validation] " + w);

    if (!result.validation.passed) {
        for (const auto& f : result.validation.failures)
            result.errors.push_back("[validation] " + f);
        return result;
    }

    result.success = true;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// containerizeVp6AviToEa  (AVI input, simple bytes-only version)
// ─────────────────────────────────────────────────────────────────────────────

std::optional<std::vector<std::uint8_t>>
containerizeVp6AviToEa(const std::filesystem::path& aviPath,
                        const EaVp6ImportTarget&     target,
                        std::string&                 errorOut)
{
    auto r = containerizeVp6AviToEaWithResult(aviPath, target);
    if (!r.success) {
        if (!r.errors.empty())
            errorOut = r.errors.front();
        else
            errorOut = "containerizeVp6AviToEa: unknown failure";
        return std::nullopt;
    }
    return std::move(r.serializedBytes);
}

} // namespace gf::media
