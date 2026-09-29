#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

namespace phonecam {

// A completed Opus frame reassembled from RTP packets.
struct OpusFrame {
    std::vector<uint8_t> payload;   // whole Opus frame (one or more Opus packets)
    uint32_t rtpTimestamp = 0;      // 48kHz RTP timestamp
    uint16_t sequenceNumber = 0;    // RTP sequence number
};

// Reassembles Opus frames from RTP packets per RFC 7587.
// Not thread-safe; create one per session and call pushRtp() on each incoming
// RTP packet (raw RTP packet including header).
class OpusDepayloader {
public:
    OpusDepayloader() = default;

    // Called for each raw RTP packet. Returns a complete Opus frame when
    // available, std::nullopt otherwise (more packets needed or frame dropped).
    // Returns frame when RTP marker bit (M) is set.
    std::optional<OpusFrame> pushRtp(const uint8_t* pkt, size_t len);

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