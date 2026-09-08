#include "qiran/svc/svc_crc.h"

static const uint32_t k_nibble[16] = {
    0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
    0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
    0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
    0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU
};

uint32_t svc_crc32_update(uint32_t crc, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;

    if (p == NULL) {
        return crc;
    }

    for (i = 0U; i < len; i++) {
        crc ^= (uint32_t)p[i];
        crc = k_nibble[crc & 0x0FU] ^ (crc >> 4);
        crc = k_nibble[crc & 0x0FU] ^ (crc >> 4);
    }

    return crc;
}

uint32_t svc_crc32_finish(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFU;
}

uint32_t svc_crc32(const void *data, uint32_t len)
{
    return svc_crc32_finish(svc_crc32_update(SVC_CRC32_INIT, data, len));
}
