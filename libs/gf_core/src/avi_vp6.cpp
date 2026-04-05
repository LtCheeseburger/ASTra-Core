#include "gf/media/avi_vp6.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace gf::media {

// ─────────────────────────────────────────────────────────────────────────────
// AVI binary read helpers  (AVI/RIFF is little-endian throughout)
// ─────────────────────────────────────────────────────────────────────────────

static bool readBytes(std::istream& in, char* buf, std::streamsize n) noexcept
{
    return static_cast<bool>(in.read(buf, n));
}

static std::uint32_t readU32le(std::istream& in, bool& ok) noexcept
{
    std::uint8_t b[4] = {};
    ok = ok && static_cast<bool>(in.read(reinterpret_cast<char*>(b), 4));
    return std::uint32_t(b[0])        |
          (std::uint32_t(b[1]) <<  8) |
          (std::uint32_t(b[2]) << 16) |
          (std::uint32_t(b[3]) << 24);
}

// Compare a 4-char buffer against a literal string (no NUL check needed).
static bool fcc4eq(const char a[4], const char* b) noexcept
{
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

// Align a chunk size to the next 2-byte boundary (AVI padding rule).
static std::uint32_t alignedSize(std::uint32_t n) noexcept
{
    return n + (n & 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal structures
// ─────────────────────────────────────────────────────────────────────────────

struct VideoStreamInfo {
    std::uint32_t dwRate  = 0;    // fps numerator   (from strh)
    std::uint32_t dwScale = 0;    // fps denominator (from strh)
    std::uint32_t dwLength = 0;   // total frame count declared in strh
    bool          hasVp6  = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// AviVp6Parser::parse
// ─────────────────────────────────────────────────────────────────────────────

std::optional<AviVp6VideoStream>
AviVp6Parser::parse(const std::filesystem::path& inputPath,
                    std::string& errorOut) const
{
    std::ifstream in(inputPath, std::ios::binary);
    if (!in.is_open()) {
        errorOut = "AviVp6Parser: cannot open: " + inputPath.string();
        return std::nullopt;
    }

    // ── RIFF header (12 bytes: "RIFF" + size + "AVI ") ───────────────────────
    {
        char riff[4], form[4];
        bool ok = true;
        if (!readBytes(in, riff, 4)) { errorOut = "AviVp6Parser: read error on RIFF id"; return std::nullopt; }
        /* uint32_t riffSize = */ readU32le(in, ok);
        if (!readBytes(in, form, 4)) { errorOut = "AviVp6Parser: read error on form type"; return std::nullopt; }
        if (!fcc4eq(riff, "RIFF") || !fcc4eq(form, "AVI ")) {
            errorOut = "AviVp6Parser: not an AVI file (invalid RIFF/AVI signature in "
                       + inputPath.string() + ")";
            return std::nullopt;
        }
    }

    // ── Walk top-level RIFF chunks ────────────────────────────────────────────
    // We need:
    //   LIST 'hdrl'  → avih + strl → strh (fps, codec)
    //   LIST 'movi'  → video frame data
    //   'idx1'       → keyframe flags

    AviVp6VideoStream stream;
    VideoStreamInfo   vsInfo;

    // We'll do three chunks of work in a single sequential pass:
    //   1. hdrl parsing (to get fps from strh)
    //   2. movi frame extraction (video payloads)
    //   3. idx1 keyframe assignment

    // Collect raw frame entries first (without keyframe info), then assign
    // keyframe flags from idx1.  For VP6-only AVI (no audio), idx1 entries
    // and video frames map 1:1 by position.

    struct RawFrame {
        std::uint32_t moviRelOffset = 0;   // for matching idx1 entries
        std::vector<std::uint8_t> data;
    };
    std::vector<RawFrame> rawFrames;

    bool foundMovi    = false;
    bool foundIdx1    = false;
    std::streampos moviDataStart = 0;  // position after "movi" tag FourCC

    while (true) {
        char fcc[4] = {};
        bool ok = true;
        if (!readBytes(in, fcc, 4)) break;   // EOF — normal end of file
        const std::uint32_t chunkSize = readU32le(in, ok);
        if (!ok) break;

        const std::streampos dataPos = in.tellg();

        // ── LIST chunk ──────────────────────────────────────────────────────
        if (fcc4eq(fcc, "LIST")) {
            char listType[4] = {};
            if (!readBytes(in, listType, 4)) break;
            const std::streampos listContentStart = in.tellg();
            const std::uint32_t  contentSize = chunkSize > 4u ? chunkSize - 4u : 0u;

            if (fcc4eq(listType, "hdrl")) {
                // Walk hdrl sub-chunks for avih and strl → strh
                const std::streampos hdrlEnd = listContentStart +
                                               static_cast<std::streamoff>(contentSize);
                while (in.tellg() < hdrlEnd) {
                    char sc[4] = {};
                    bool ok2 = true;
                    if (!readBytes(in, sc, 4)) break;
                    const std::uint32_t scSize = readU32le(in, ok2);
                    if (!ok2) break;
                    const std::streampos scData = in.tellg();

                    if (fcc4eq(sc, "LIST")) {
                        // Inner LIST (strl = stream list)
                        char st[4] = {};
                        if (readBytes(in, st, 4) && fcc4eq(st, "strl")) {
                            // Walk strl for strh
                            const std::streampos strlEnd =
                                scData + static_cast<std::streamoff>(scSize);
                            while (in.tellg() < strlEnd) {
                                char sc2[4] = {};
                                bool ok3 = true;
                                if (!readBytes(in, sc2, 4)) break;
                                const std::uint32_t sc2Size = readU32le(in, ok3);
                                if (!ok3) break;
                                const std::streampos sc2Data = in.tellg();

                                if (fcc4eq(sc2, "strh") && sc2Size >= 36u) {
                                    // strh layout (all LE):
                                    // +00: fccType   (4)  — 'vids' for video
                                    // +04: fccHandler(4)  — codec FourCC
                                    // +08: dwFlags   (4)
                                    // +12: wPriority (2)
                                    // +14: wLanguage (2)
                                    // +16: dwInitialFrames (4)
                                    // +20: dwScale   (4)
                                    // +24: dwRate    (4)
                                    // +28: dwStart   (4)
                                    // +32: dwLength  (4)
                                    char strhBuf[36] = {};
                                    if (in.read(strhBuf, 36)) {
                                        if (fcc4eq(strhBuf, "vids")) {
                                            vsInfo.hasVp6 =
                                                strhBuf[4]=='V' && strhBuf[5]=='P' &&
                                                strhBuf[6]=='6';
                                            std::uint32_t tmp;
                                            std::memcpy(&tmp, strhBuf + 20, 4);
                                            vsInfo.dwScale = tmp;
                                            std::memcpy(&tmp, strhBuf + 24, 4);
                                            vsInfo.dwRate  = tmp;
                                            std::memcpy(&tmp, strhBuf + 32, 4);
                                            vsInfo.dwLength = tmp;
                                        }
                                    }
                                }
                                // Skip to next strh sub-chunk
                                in.seekg(sc2Data + static_cast<std::streamoff>(
                                    alignedSize(sc2Size)), std::ios::beg);
                                if (!in.good()) break;
                            }
                        }
                        // Skip rest of strl
                        in.seekg(scData + static_cast<std::streamoff>(
                            alignedSize(scSize)), std::ios::beg);
                    } else {
                        in.seekg(scData + static_cast<std::streamoff>(
                            alignedSize(scSize)), std::ios::beg);
                    }
                    if (!in.good()) break;
                }
                in.seekg(hdrlEnd, std::ios::beg);

            } else if (fcc4eq(listType, "movi")) {
                foundMovi     = true;
                moviDataStart = listContentStart;   // position right after "movi" tag
                const std::streampos moviEnd =
                    moviDataStart + static_cast<std::streamoff>(contentSize);

                // Walk movi sub-chunks to collect video frames.
                while (in.tellg() < moviEnd) {
                    char mc[4] = {};
                    bool ok2 = true;
                    if (!readBytes(in, mc, 4)) break;
                    const std::uint32_t mcSize = readU32le(in, ok2);
                    if (!ok2) break;
                    const std::streampos mcData = in.tellg();

                    // Relative offset from moviDataStart (used for idx1 matching).
                    // idx1 offsets point to the chunk FourCC, which is 8 bytes before
                    // the data position (FourCC(4) + size(4)).
                    const std::uint32_t relOff = static_cast<std::uint32_t>(
                        static_cast<std::streamoff>(mcData) - 8 -
                        static_cast<std::streamoff>(moviDataStart));

                    // Video stream 0: '00dc' (compressed) or '00db' (uncompressed).
                    // Stream indicator: first two chars '0','0'.
                    // Type indicator:   third char 'd', fourth 'c' or 'b'.
                    const bool isVideoChunk =
                        mc[0] == '0' && mc[1] == '0' &&
                        (mc[2] == 'd' || mc[2] == 'D') &&
                        (mc[3] == 'c' || mc[3] == 'C' ||
                         mc[3] == 'b' || mc[3] == 'B');

                    if (isVideoChunk && mcSize > 0u) {
                        RawFrame rf;
                        rf.moviRelOffset = relOff;
                        rf.data.resize(mcSize);
                        if (!in.read(reinterpret_cast<char*>(rf.data.data()),
                                     static_cast<std::streamsize>(mcSize)))
                        {
                            stream.warnings.push_back(
                                "AviVp6Parser: truncated video frame at movi offset " +
                                std::to_string(relOff) + "; skipping");
                            break;
                        }
                        rawFrames.push_back(std::move(rf));
                    } else {
                        // Skip audio, empty, or unrecognised chunks.
                    }

                    // Seek to next chunk (aligned).
                    in.seekg(mcData + static_cast<std::streamoff>(
                        alignedSize(mcSize)), std::ios::beg);
                    if (!in.good()) break;
                }
                in.seekg(moviEnd, std::ios::beg);

            } else {
                // Unknown LIST — skip.
                in.seekg(dataPos + static_cast<std::streamoff>(
                    alignedSize(chunkSize)), std::ios::beg);
            }

        // ── idx1 chunk ───────────────────────────────────────────────────────
        } else if (fcc4eq(fcc, "idx1")) {
            foundIdx1 = true;

            // idx1 entry: ckid(4) + dwFlags(4) + dwOffset(4) + dwChunkLength(4)
            // AVIIF_KEYFRAME = 0x00000010
            static constexpr std::uint32_t AVIIF_KEYFRAME = 0x00000010u;

            const std::uint32_t entryCount = chunkSize / 16u;

            // Collect keyframe flags for video stream 0 entries in order.
            std::vector<bool> kfFlags;
            kfFlags.reserve(rawFrames.size());

            for (std::uint32_t i = 0; i < entryCount && kfFlags.size() <= rawFrames.size(); ++i) {
                char ckid[4] = {};
                bool ok2 = true;
                if (!readBytes(in, ckid, 4)) break;
                const std::uint32_t dwFlags  = readU32le(in, ok2);
                /* uint32_t dwOffset = */ readU32le(in, ok2);
                /* uint32_t dwChunkLength = */ readU32le(in, ok2);
                if (!ok2) break;

                // Only collect entries for video stream 0 ('00dc' or '00db').
                if (ckid[0]=='0' && ckid[1]=='0' &&
                    (ckid[2]=='d' || ckid[2]=='D') &&
                    (ckid[3]=='c' || ckid[3]=='C' ||
                     ckid[3]=='b' || ckid[3]=='B'))
                {
                    kfFlags.push_back((dwFlags & AVIIF_KEYFRAME) != 0u);
                }
            }

            // Assign keyframe flags to raw frames by position.
            for (std::size_t fi = 0; fi < rawFrames.size(); ++fi) {
                if (fi < kfFlags.size()) {
                    // kfFlags[fi] assigned below when building FlvVideoPackets.
                    // Store in a parallel vector for now — we'll combine shortly.
                    (void)kfFlags[fi]; // suppress unused
                }
            }

            // Store flags alongside frames (resize to match).
            // Re-walk: assign directly during packet construction.
            // Build packets here using kfFlags:
            stream.packets.reserve(rawFrames.size());
            for (std::size_t fi = 0; fi < rawFrames.size(); ++fi) {
                FlvVideoPacket pkt;
                pkt.isKeyframe = (fi < kfFlags.size()) ? kfFlags[fi] : (fi == 0);
                pkt.payload    = std::move(rawFrames[fi].data);

                // Synthetic timestamp from fps.
                if (vsInfo.dwRate > 0u && vsInfo.dwScale > 0u) {
                    pkt.timestampMs = static_cast<std::uint32_t>(
                        std::round(static_cast<double>(fi) *
                                   static_cast<double>(vsInfo.dwScale) * 1000.0 /
                                   static_cast<double>(vsInfo.dwRate)));
                } else {
                    pkt.timestampMs = static_cast<std::uint32_t>(fi);
                }

                stream.packets.push_back(std::move(pkt));
            }
            rawFrames.clear();   // ownership transferred

            in.seekg(dataPos + static_cast<std::streamoff>(
                alignedSize(chunkSize)), std::ios::beg);

        } else {
            // Unknown top-level chunk (JUNK padding, etc.) — skip.
            in.seekg(dataPos + static_cast<std::streamoff>(
                alignedSize(chunkSize)), std::ios::beg);
        }

        if (!in.good()) break;
    }

    // ── If idx1 was not found, build packets from rawFrames with first=keyframe
    if (!foundIdx1 && !rawFrames.empty()) {
        stream.warnings.push_back(
            "AviVp6Parser: no idx1 index found; keyframe detection unavailable — "
            "treating first frame as keyframe, all others as delta frames");
        stream.packets.reserve(rawFrames.size());
        for (std::size_t fi = 0; fi < rawFrames.size(); ++fi) {
            FlvVideoPacket pkt;
            pkt.isKeyframe  = (fi == 0);
            pkt.payload     = std::move(rawFrames[fi].data);

            if (vsInfo.dwRate > 0u && vsInfo.dwScale > 0u) {
                pkt.timestampMs = static_cast<std::uint32_t>(
                    std::round(static_cast<double>(fi) *
                               static_cast<double>(vsInfo.dwScale) * 1000.0 /
                               static_cast<double>(vsInfo.dwRate)));
            } else {
                pkt.timestampMs = static_cast<std::uint32_t>(fi);
            }

            stream.packets.push_back(std::move(pkt));
        }
    }

    // ── Post-parse validation ─────────────────────────────────────────────────
    if (!foundMovi) {
        errorOut = "AviVp6Parser: no movi LIST found in AVI file";
        return std::nullopt;
    }
    if (stream.packets.empty()) {
        errorOut = "AviVp6Parser: no video frames found in AVI movi LIST";
        return std::nullopt;
    }
    if (!vsInfo.hasVp6) {
        stream.warnings.push_back(
            "AviVp6Parser: video stream handler FourCC is not VP6x; "
            "the file may not contain VP6 video — containerization may fail");
    }

    stream.frameCount = static_cast<std::uint32_t>(stream.packets.size());

    // Compute fps from strh (authoritative for AVI — frames have no timestamps).
    if (vsInfo.dwRate > 0u && vsInfo.dwScale > 0u)
        stream.fps = static_cast<double>(vsInfo.dwRate) /
                     static_cast<double>(vsInfo.dwScale);

    // Sanity checks.
    if (!stream.packets.front().isKeyframe) {
        stream.warnings.push_back(
            "AviVp6Parser: first video frame is not a keyframe; "
            "EA VP6 files are expected to start with an MV0K chunk");
    }
    const bool hasKf = std::any_of(
        stream.packets.begin(), stream.packets.end(),
        [](const FlvVideoPacket& p){ return p.isKeyframe; });
    if (!hasKf) {
        stream.warnings.push_back(
            "AviVp6Parser: no keyframes found among " +
            std::to_string(stream.frameCount) + " extracted frames; "
            "idx1 may be missing or all frames may be flagged as inter-frames");
    }

    stream.valid = true;
    return stream;
}

} // namespace gf::media
