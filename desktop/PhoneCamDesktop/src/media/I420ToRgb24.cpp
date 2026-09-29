// I420ToRgb24.cpp
//
// Integer BT.601 limited-range YUV420 to RGB24 conversion.
// Uses the standard ITU-R BT.601 studio-swing equations expressed in
// fixed-point arithmetic.

#include "I420ToRgb24.h"

namespace {

inline int clamp255(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

} // namespace

namespace phonecam {

void i420ToRgb24(const uint8_t* srcY, int strideY,
                 const uint8_t* srcU, int strideU,
                 const uint8_t* srcV, int strideV,
                 uint8_t* dst,
                 int width, int height) {
    // BT.601 limited range:
    //   C = Y - 16, D = U - 128, E = V - 128
    //   R = ( 298*C           + 409*E + 128) >> 8
    //   G = ( 298*C - 100*D - 208*E + 128) >> 8
    //   B = ( 298*C + 516*D           + 128) >> 8
    const int dstStride = width * 3;
    for (int y = 0; y < height; ++y) {
        const uint8_t* yRow = srcY + y * strideY;
        const uint8_t* uRow = srcU + (y >> 1) * strideU;
        const uint8_t* vRow = srcV + (y >> 1) * strideV;
        uint8_t* out = dst + y * dstStride;
        for (int x = 0; x < width; ++x) {
            const int C = static_cast<int>(yRow[x]) - 16;
            const int D = static_cast<int>(uRow[x >> 1]) - 128;
            const int E = static_cast<int>(vRow[x >> 1]) - 128;
            const int y298 = 298 * C;
            const int r = (y298            + 409 * E + 128) >> 8;
            const int g = (y298 - 100 * D - 208 * E + 128) >> 8;
            const int b = (y298 + 516 * D           + 128) >> 8;
            out[x * 3 + 0] = static_cast<uint8_t>(clamp255(r));
            out[x * 3 + 1] = static_cast<uint8_t>(clamp255(g));
            out[x * 3 + 2] = static_cast<uint8_t>(clamp255(b));
        }
    }
}

} // namespace phonecam
