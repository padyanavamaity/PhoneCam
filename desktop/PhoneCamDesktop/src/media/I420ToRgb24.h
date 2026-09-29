#pragma once

#include <cstdint>
#include <cstddef>

namespace phonecam {

// Convert planar I420 (YUV 4:2:0 limited range, BT.601) to packed RGB24
// (R,G,B byte order, top-down rows, stride = width*3).
//
// - srcY/srcU/srcV: planar Y, U, V pointers.
// - strideY/strideU/strideV: bytes per row of each plane.
// - dst: caller-provided buffer of at least width*height*3 bytes.
// - width/height: picture dimensions (must be even for I420; if odd, the
//   chroma sampling is treated as ceil(w/2)/ceil(h/2)).
//
// Uses fixed-point integer math; no floating point.
// This is NOT a fast path -- it is correct and simple, intended for a
// preview window. For production broadcast use a SIMD/GPU-based converter.
void i420ToRgb24(const uint8_t* srcY, int strideY,
                 const uint8_t* srcU, int strideU,
                 const uint8_t* srcV, int strideV,
                 uint8_t* dst,
                 int width, int height);

} // namespace phonecam
