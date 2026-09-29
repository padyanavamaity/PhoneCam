#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

// This translation unit is only built when PHONECAM_HAVE_VPX is defined
// (i.e., libvpx was found on the build host). It is NOT compiled in
// depay-only mode.

#ifdef PHONECAM_HAVE_VPX

namespace phonecam {

struct DecodedI420 {
    // Y plane: width*height bytes
    // U, V planes: ((width+1)/2)*((height+1)/2) bytes each
    // Planes are tightly packed (stride == width for Y; == (width+1)/2 for U/V).
    std::unique_ptr<uint8_t[]> data;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Stateless-per-call VP8 decoder using libvpx.
// The decoder object holds vpx_codec_ctx_t internally.
class Vp8Decoder {
public:
    Vp8Decoder();
    ~Vp8Decoder();

    // Non-copyable, non-movable (codec context is not safely relocatable)
    Vp8Decoder(const Vp8Decoder&) = delete;
    Vp8Decoder& operator=(const Vp8Decoder&) = delete;

    // Decode a complete VP8 frame.
    // Returns nullptr on decode failure or no frame produced.
    std::unique_ptr<DecodedI420> decode(const uint8_t* data, size_t size);

private:
    struct Impl;
    Impl* impl_;  // raw pointer owned by this; deleted in destructor
};

} // namespace phonecam

#endif // PHONECAM_HAVE_VPX
