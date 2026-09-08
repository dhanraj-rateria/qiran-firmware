#ifndef QIRAN_COMM_BYTES_H
#define QIRAN_COMM_BYTES_H

#include "qiran/qiran_types.h"

/*
 * Frames are packed most significant byte first. The byte order is not stated
 * in the frame definition; this follows the convention of the space packet
 * standard used on the other link, so both links agree.
 *
 * OPEN: confirm byte order with the interface owner.
 *
 * Packing is explicit rather than by structure overlay, so the layout does not
 * depend on the compiler's padding or on the processor's byte order.
 */
static inline uint32_t comm_put_u8(uint8_t *buf, uint32_t at, uint8_t v)
{
    buf[at] = v;
    return at + 1U;
}

static inline uint32_t comm_put_u16(uint8_t *buf, uint32_t at, uint16_t v)
{
    buf[at] = (uint8_t)(v >> 8);
    buf[at + 1U] = (uint8_t)(v & 0xFFU);
    return at + 2U;
}

static inline uint32_t comm_put_u32(uint8_t *buf, uint32_t at, uint32_t v)
{
    buf[at] = (uint8_t)(v >> 24);
    buf[at + 1U] = (uint8_t)((v >> 16) & 0xFFU);
    buf[at + 2U] = (uint8_t)((v >> 8) & 0xFFU);
    buf[at + 3U] = (uint8_t)(v & 0xFFU);
    return at + 4U;
}

static inline uint32_t comm_put_i16(uint8_t *buf, uint32_t at, int16_t v)
{
    return comm_put_u16(buf, at, (uint16_t)v);
}

static inline uint16_t comm_get_u16(const uint8_t *buf, uint32_t at)
{
    return (uint16_t)(((uint16_t)buf[at] << 8) | (uint16_t)buf[at + 1U]);
}

static inline uint32_t comm_get_u32(const uint8_t *buf, uint32_t at)
{
    return ((uint32_t)buf[at] << 24) | ((uint32_t)buf[at + 1U] << 16) |
           ((uint32_t)buf[at + 2U] << 8) | (uint32_t)buf[at + 3U];
}

#endif
