#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

namespace phonecam {

// A completed VP8 frame reassembled from RTP packets.
struct Vp8Frame {
    std::vector<uint8_t> payload;   // whole VP8 frame (keyframes start with start-of-partition 0)
    uint32_t rtpTimestamp = 0;      // 90kHz RTP timestamp
};

// Reassembles VP8 bitstream frames from RTP packet bytes (RFC 7741).
// Not thread-safe; create one per session and call pushRtp() on each incoming
// RTP packet (raw RTP packet including header).
class Vp8Depayloader {
public:
    Vp8Depayloader() = default;

    // Called for each raw RTP packet. Returns a complete VP8 frame when
    // available, std::nullopt otherwise (more packets needed or frame dropped).
    std::optional<Vp8Frame> pushRtp(const uint8_t* pkt, size_t len);

    // Reset internal state (e.g. after keyframe request or session restart).
    void reset();

private:
    std::vector<uint8_t> inProgress_;
    uint32_t expectedTimestamp_ = 0;
    bool haveTimestamp_ = false;
    uint16_t lastSeq_ = 0;
    bool lastSeqValid_ = false;
    bool sawStart_ = false;
};

} // namespace phonecam
