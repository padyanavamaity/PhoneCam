// Vp8Depayloader.cpp
//
// Reassembles VP8 frames from RTP packets per RFC 7741.
// Stateful per session. Packets may be lost; an incomplete frame is dropped.
//
// VP8 Payload Descriptor (RFC 7741 §4.2):
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//   +-+-+-+-+-+-+-+-+
//   |X|R|N|S|R| PID |   (REQUIRED)
//   +-+-+-+-+-+-+-+-+
// ...
// S=Start of VP8 partition; PID=partition index.
//
// Frame boundary condition: An RTP packet with the M (marker) bit set in its
// RTP header marks the last packet of a VP8 frame.

#include "Vp8Depayloader.h"

#include <cstring>

namespace phonecam {

void Vp8Depayloader::reset() {
    inProgress_.clear();
    expectedTimestamp_ = 0;
    haveTimestamp_ = false;
    lastSeq_ = 0;
    lastSeqValid_ = false;
    sawStart_ = false;
}

// Parse the VP8 payload descriptor length; returns the offset at which the
// VP8 payload bytes begin, or 0 on error.
static size_t vp8PayloadDescLength(const uint8_t* p, size_t len) {
    if (len < 1) return 0;
    const uint8_t X = (p[0] & 0x80) >> 7;
    size_t off = 1;
    if (X) {
        if (len < off + 1) return 0;
        const uint8_t I = (p[off] & 0x80) >> 7;
        const uint8_t L = (p[off] & 0x40) >> 6;
        const uint8_t T = (p[off] & 0x20) >> 5;
        const uint8_t K = (p[off] & 0x10) >> 4;
        ++off;
        if (I) {
            if (len < off + 1) return 0;
            if (p[off] & 0x80) {
                if (len < off + 2) return 0;
                off += 2; // 16-bit PictureID
            } else {
                off += 1; // 7-bit PictureID
            }
        }
        if (L) { if (len < off + 1) return 0; off += 1; }
        if (T || K) { if (len < off + 1) return 0; off += 1; }
    }
    return off;
}

std::optional<Vp8Frame> Vp8Depayloader::pushRtp(const uint8_t* pkt, size_t len) {
    // RTP fixed header: 12 bytes minimum
    if (len < 12) return std::nullopt;

    const uint8_t V = (pkt[0] & 0xC0) >> 6;
    if (V != 2) return std::nullopt;

    const uint8_t X = (pkt[0] & 0x10) >> 4;
    const uint8_t CC = pkt[0] & 0x0F;
    const uint8_t M = (pkt[1] & 0x80) >> 7;
    // pkt[1] & 0x7F is the payload type (PT); we assume VP8's PT (e.g. 96)
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

    const size_t payDescLen = vp8PayloadDescLength(pkt + hlen, len - hlen);
    if (payDescLen == 0) {
        reset();
        return std::nullopt;
    }

    const uint8_t* payloadStart = pkt + hlen + payDescLen;
    const size_t payloadLen = (len - hlen) - payDescLen;

    const bool S = (pkt[hlen] & 0x10) != 0;
    // const uint8_t PID = pkt[hlen] & 0x07; // unused; libvpx reassembles partitions

    // Out-of-order or wrapped sequence within the same timestamp -> drop and reset
    if (lastSeqValid_ && haveTimestamp_ && ts == expectedTimestamp_ && !inProgress_.empty()) {
        int16_t d = static_cast<int16_t>(seq - lastSeq_);
        if (d <= 0 || d > 1000) {
            reset();
        }
    }

    if (S) {
        // Start of partition 0: begin a new frame assembly
        inProgress_.clear();
        expectedTimestamp_ = ts;
        haveTimestamp_ = true;
        sawStart_ = true;
        inProgress_.insert(inProgress_.end(), payloadStart, payloadStart + payloadLen);
    } else if (sawStart_ && haveTimestamp_ && ts == expectedTimestamp_) {
        inProgress_.insert(inProgress_.end(), payloadStart, payloadStart + payloadLen);
    } else {
        // Packet for a new timestamp without start bit, or stale packet
        // Drop and wait for next keyframe
        return std::nullopt;
    }

    lastSeq_ = seq;
    lastSeqValid_ = true;

    if (M) {
        // End of frame
        if (inProgress_.empty()) {
            reset();
            return std::nullopt;
        }
        Vp8Frame f;
        f.payload = std::move(inProgress_);
        f.rtpTimestamp = expectedTimestamp_;
        reset();
        return f;
    }

    return std::nullopt;
}

} // namespace phonecam
