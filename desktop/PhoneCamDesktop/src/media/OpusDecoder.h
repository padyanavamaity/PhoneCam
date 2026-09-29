#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

// This translation unit is only built when PHONECAM_HAVE_OPUS is defined
// (i.e., libopus was found on the build host). It is NOT compiled in
// depay-only mode.

#ifdef PHONECAM_HAVE_OPUS

namespace phonecam {

// Decoded Opus audio (PCM float samples, interleaved stereo at 48kHz)
struct DecodedOpus {
    std::vector<float> samples;    // Interleaved stereo: L R L R ...
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    uint32_t frames = 0;           // Number of frames per channel
};

// Stateless-per-call Opus decoder using libopus.
// The decoder object holds OpusDecoder* internally.
class OpusDecoder {
public:
    OpusDecoder();
    ~OpusDecoder();

    // Non-copyable, non-movable (decoder state is not safely relocatable)
    OpusDecoder(const OpusDecoder&) = delete;
    OpusDecoder& operator=(const OpusDecoder&) = delete;

    // Decode an Opus frame (can contain multiple Opus packets).
    // Returns nullptr on decode failure or no frame produced.
    // Output is always 48kHz stereo float samples in [-1, 1] range.
    std::unique_ptr<DecodedOpus> decode(const uint8_t* data, size_t size);

private:
    struct Impl;
    Impl* impl_;  // raw pointer owned by this; deleted in destructor
};

} // namespace phonecam

#endif // PHONECAM_HAVE_OPUS