// OpusDepayloader.cpp
//
// Reassembles Opus frames from RTP packets per RFC 7587.
// Stateful per session. Packets may be lost; an incomplete frame is dropped.
//
// Opus RTP Payload Format (RFC 7587):
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |V=2|P|X|  CC   |M|     PT      |       sequence number         |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |                           timestamp                           |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |           synchronization source (SSRC) identifier            |
//   +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
//   |      contributing source (CSRC) identifiers (if any)          |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |      RTP header extension (if X=1)                            |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |                      Opus payload(s)                          |
//   |                                                               |
//   |                      ...                                      |
//
// Opus frame boundary condition: An RTP packet with the M (marker) bit
// set in its RTP header marks the last packet of an Opus frame.
// Multiple Opus packets may be packed into a single RTP packet (not handled here
// for simplicity - we treat entire RTP payload as one Opus frame when M=1).

#include "OpusDepayloader.h"

#include <cstring>

namespace phonecam {

void OpusDepayloader::reset() {
    inProgress_.clear();
    expectedTimestamp_ = 0;
    haveTimestamp_ = false;
    lastSeq_ = 0;
    lastSeqValid_ = false;
    sawStart_ = false;
}

std::optional<OpusFrame> OpusDepayloader::pushRtp(const uint8_t* pkt, size_t len) {
    // RTP fixed header: 12 bytes minimum
    if (len < 12) return std::nullopt;

    const uint8_t V = (pkt[0] & 0xC0) >> 6;
    if (V != 2) return std::nullopt;

    const uint8_t X = (pkt[0] & 0x10) >> 4;
    const uint8_t CC = pkt[0] & 0x0F;
    const uint8_t M = (pkt[1] & 0x80) >> 7;
    // pkt[1] & 0x7F is the payload type (PT); we assume Opus PT (e.g. 111)
    const uint16_t seq = static_cast<uint16_t>((pkt[2] << 8) | pkt[3]);
    const uint32_t ts = (static_cast<uint32_t>(pkt[4]) << 24) |
                        (static_cast<uint32_t>(pkt[5]) << 16) |
                        (static_cast<uint32_t>(pkt[6]) <<  8) |
                        (static_cast<uint32_t>(pkt[7]) <<  0);

    size_t hlen = 12 + 4 * CC;
    if (len < hlen) return std::nullopt;
    if (X) {
        if (len < hlen + 4) return std::nullopt;
        const uint16_t extLenWords = static_cast<uint16_t>(
            (pkt[hlen + 2] << 8) | pkt[hlen + 3]);
        hlen += 4 + 4 * extLenWords;
        if (len < hlen) return std::nullopt;
    }

    const size_t payloadLen = len - hlen;
    if (payloadLen == 0) return std::nullopt;  // No Opus payload

    const uint8_t* payloadStart = pkt + hlen;

    // Out-of-order or wrapped sequence within the same timestamp -> drop and reset
    if (lastSeqValid_ && haveTimestamp_ && ts == expectedTimestamp_ && !inProgress_.empty()) {
        int16_t d = static_cast<int16_t>(seq - lastSeq_);
        if (d <= 0 || d > 1000) {
            reset();
        }
    }

    if (!sawStart_ || !haveTimestamp_ || ts != expectedTimestamp_) {
        // Start of a new frame assembly (new timestamp)
        inProgress_.clear();
        expectedTimestamp_ = ts;
        haveTimestamp_ = true;
        sawStart_ = true;
    }

    // Append Opus payload to in-progress frame
    inProgress_.insert(inProgress_.end(), payloadStart, payloadStart + payloadLen);

    lastSeq_ = seq;
    lastSeqValid_ = true;

    if (M) {
        // End of frame (M marker bit set)
        if (inProgress_.empty()) {
            reset();
            return std::nullopt;
        }
        OpusFrame f;
        f.payload = std::move(inProgress_);
        f.rtpTimestamp = expectedTimestamp_;
        f.sequenceNumber = seq;
        reset();
        return f;
    }

    return std::nullopt;
}

} // namespace phonecam