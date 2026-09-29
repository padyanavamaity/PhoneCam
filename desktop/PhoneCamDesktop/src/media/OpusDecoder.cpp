// OpusDecoder.cpp
//
// Opus decoder using libopus.
// Only compiled when PHONECAM_HAVE_OPUS is defined (libopus found).

#ifdef PHONECAM_HAVE_OPUS

#include "OpusDecoder.h"

#include <opus/opus.h>
#include <memory>
#include <vector>
#include <algorithm>

namespace phonecam {

struct OpusDecoder::Impl {
    OpusDecoder* decoder = nullptr;
    
    Impl() {
        int error = 0;
        decoder = opus_decoder_create(48000, 2, &error);
        if (error != OPUS_OK || !decoder) {
            decoder = nullptr;
        }
    }
    
    ~Impl() {
        if (decoder) {
            opus_decoder_destroy(decoder);
            decoder = nullptr;
        }
    }
};

OpusDecoder::OpusDecoder() : impl_(new Impl()) {
}

OpusDecoder::~OpusDecoder() {
    delete impl_;
}

std::unique_ptr<DecodedOpus> OpusDecoder::decode(const uint8_t* data, size_t size) {
    if (!impl_->decoder || size == 0) {
        return nullptr;
    }
    
    // Opus can decode up to 120ms of audio per frame at 48kHz = 5760 samples per channel
    // For stereo: 5760 * 2 = 11520 float samples max
    constexpr int MAX_FRAME_SIZE = 5760;
    std::vector<float> pcm(MAX_FRAME_SIZE * 2);
    
    // Decode the Opus frame - libopus handles multiple packets in one payload
    int frames = opus_decode_float(impl_->decoder, data, static_cast<int32_t>(size),
                                   pcm.data(), MAX_FRAME_SIZE, 0);
    
    if (frames < 0) {
        // Decode error
        return nullptr;
    }
    
    if (frames == 0) {
        // No audio produced (DTX / comfort noise)
        return nullptr;
    }
    
    auto result = std::make_unique<DecodedOpus>();
    result->sampleRate = 48000;
    result->channels = 2;
    result->frames = static_cast<uint32_t>(frames);
    result->samples.resize(static_cast<size_t>(frames) * 2);
    
    // Copy only the valid samples
    std::copy(pcm.begin(), pcm.begin() + frames * 2, result->samples.begin());
    
    return result;
}

} // namespace phonecam

#endif // PHONECAM_HAVE_OPUS