#ifndef QIRAN_SVC_CRC_H
#define QIRAN_SVC_CRC_H

#include "qiran/qiran_types.h"

/*
 * CRC-32 as used by the common reflected variant: polynomial 0xEDB88320,
 * initial value all ones, final complement. Its check value over the nine
 * bytes "123456789" is 0xCBF43926, which the unit tests assert against so this
 * is pinned to the published definition rather than only to itself.
 *
 * Implemented over a sixteen-entry table: two lookups per byte, sixty-four
 * bytes of constant data, no run-time initialisation.
 */
#define SVC_CRC32_INIT 0xFFFFFFFFU

uint32_t svc_crc32(const void *data, uint32_t len);

/* Incremental form, for a checksum spanning several buffers. Seed the first
   call with SVC_CRC32_INIT and complement the result once at the end. */
uint32_t svc_crc32_update(uint32_t crc, const void *data, uint32_t len);
uint32_t svc_crc32_finish(uint32_t crc);

#endif
