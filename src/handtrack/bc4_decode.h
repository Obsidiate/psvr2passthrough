#pragma once
#include <cstdint>

namespace psvr2pt {

// Decode a single BC4_UNORM 4x4 block (8 bytes) into a 4x4 uint8 patch.
// dst_pitch: number of bytes between rows in the output buffer.
inline void decode_bc4_block(const uint8_t* src, uint8_t* dst, int dst_pitch) {
    const uint8_t r0 = src[0], r1 = src[1];

    uint8_t pal[8];
    pal[0] = r0;
    pal[1] = r1;
    if (r0 > r1) {
        pal[2] = static_cast<uint8_t>((6*r0 + 1*r1 + 3) / 7);
        pal[3] = static_cast<uint8_t>((5*r0 + 2*r1 + 3) / 7);
        pal[4] = static_cast<uint8_t>((4*r0 + 3*r1 + 3) / 7);
        pal[5] = static_cast<uint8_t>((3*r0 + 4*r1 + 3) / 7);
        pal[6] = static_cast<uint8_t>((2*r0 + 5*r1 + 3) / 7);
        pal[7] = static_cast<uint8_t>((1*r0 + 6*r1 + 3) / 7);
    } else {
        pal[2] = static_cast<uint8_t>((4*r0 + 1*r1 + 2) / 5);
        pal[3] = static_cast<uint8_t>((3*r0 + 2*r1 + 2) / 5);
        pal[4] = static_cast<uint8_t>((2*r0 + 3*r1 + 2) / 5);
        pal[5] = static_cast<uint8_t>((1*r0 + 4*r1 + 2) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }

    // 16 × 3-bit indices packed in bytes [2..7], little-endian.
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= static_cast<uint64_t>(src[2 + i]) << (i * 8);

    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            dst[row * dst_pitch + col] = pal[(bits >> ((row * 4 + col) * 3)) & 7];
}

// Decode a full BC4_UNORM image of (width × height) pixels.
// src: kBC4DataSize bytes (width/4 × height/4 blocks × 8 bytes/block).
// dst: width × height bytes, row-major.
// width and height must be multiples of 4.
inline void decode_bc4(const uint8_t* src, uint8_t* dst, int width, int height) {
    const int bx = width  / 4;
    const int by = height / 4;
    for (int r = 0; r < by; ++r)
        for (int c = 0; c < bx; ++c)
            decode_bc4_block(src + (r * bx + c) * 8,
                             dst  + (r * 4) * width + c * 4,
                             width);
}

} // namespace psvr2pt