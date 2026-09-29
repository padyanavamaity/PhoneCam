// Vp8Decoder.cpp
// Only built when PHONECAM_HAVE_VPX is defined.
// See CMakeLists.txt: libvpx requires Perl+configure to build from source,
// which is unavailable on this host, so this file will usually not be part
// of the build. It IS included so the code path exists for hosts with
// vcpkg-installed libvpx.

#ifdef PHONECAM_HAVE_VPX

#include "Vp8Decoder.h"
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#include <cstring>

namespace phonecam {

struct Vp8Decoder::Impl {
    vpx_codec_ctx_t ctx{};
    bool initialized = false;

    Impl() {
        vpx_codec_dec_cfg_t cfg{};
        cfg.threads = 1;
        cfg.w = 0;
        cfg.h = 0;
        if (vpx_codec_dec_init(&ctx, vpx_codec_vp8_dx(), &cfg, 0) == VPX_CODEC_OK) {
            initialized = true;
        }
    }

    ~Impl() {
        if (initialized) {
            vpx_codec_destroy(&ctx);
        }
    }
};

Vp8Decoder::Vp8Decoder() : impl_(new Impl()) {}
Vp8Decoder::~Vp8Decoder() { delete impl_; }

std::unique_ptr<DecodedI420> Vp8Decoder::decode(const uint8_t* data, size_t size) {
    if (!impl_ || !impl_->initialized || !data || size == 0) {
        return nullptr;
    }
    if (vpx_codec_decode(&impl_->ctx, data, static_cast<unsigned int>(size), nullptr, 0) != VPX_CODEC_OK) {
        return nullptr;
    }
    vpx_codec_iter_t iter = nullptr;
    vpx_image_t* img = vpx_codec_get_frame(&impl_->ctx, &iter);
    if (!img || img->fmt != VPX_IMG_FMT_I420) {
        return nullptr;
    }
    auto out = std::make_unique<DecodedI420>();
    out->width = img->d_w;
    out->height = img->d_h;
    const size_t ySize = static_cast<size_t>(out->width) * out->height;
    const size_t uvW = (out->width + 1) / 2;
    const size_t uvH = (out->height + 1) / 2;
    const size_t uvSize = uvW * uvH;
    out->data = std::make_unique<uint8_t[]>(ySize + 2 * uvSize);

    // Copy Y
    uint8_t* dst = out->data.get();
    for (uint32_t r = 0; r < out->height; ++r) {
        std::memcpy(dst + r * out->width, img->planes[0] + r * img->stride[0], out->width);
    }
    // Copy U
    dst = out->data.get() + ySize;
    for (uint32_t r = 0; r < uvH; ++r) {
        std::memcpy(dst + r * uvW, img->planes[1] + r * img->stride[1], uvW);
    }
    // Copy V
    dst = out->data.get() + ySize + uvSize;
    for (uint32_t r = 0; r < uvH; ++r) {
        std::memcpy(dst + r * uvW, img->planes[2] + r * img->stride[2], uvW);
    }
    return out;
}

} // namespace phonecam

#endif // PHONECAM_HAVE_VPX
