#ifndef QIRAN_STORAGE_NAND_H
#define QIRAN_STORAGE_NAND_H

#include "qiran/comm/comm_ccsds.h"
#include "qiran/qiran_types.h"

/*
 * Non-volatile store for one run's framed products, and the transfer of them
 * to the observer.
 *
 * Records are self-describing and individually checksummed, so a store that
 * was interrupted or a page that has degraded costs the records it damaged
 * rather than everything after them.
 */
typedef enum {
    NAND_RECORD_RAW = 0,
    NAND_RECORD_PROCESSED,
    NAND_RECORD_HEALTH,
    NAND_RECORD_COUNT
} nand_record_type_t;

#define NAND_RECORD_MAGIC  0x514E4452U
#define NAND_HEADER_BYTES  20U

typedef struct {
    qiran_status_t (*erase)(void *ctx, uint32_t offset, uint32_t bytes);
    qiran_status_t (*write)(void *ctx, uint32_t offset, const void *src,
                            uint32_t len);
    qiran_status_t (*read)(void *ctx, uint32_t offset, void *dst, uint32_t len);
    uint32_t       capacity_bytes;
    void          *ctx;
} storage_nand_ops_t;

void storage_nand_init(void);

qiran_status_t storage_nand_set_ops(const storage_nand_ops_t *ops);
bool           storage_nand_ready(void);

qiran_status_t storage_nand_store(nand_record_type_t type, const void *data,
                                  uint32_t len);

/*
 * Starts sending the stored records. The transfer runs a record at a time from
 * the cyclic loop rather than in one go, because a run's data takes seconds to
 * move and the loop has twenty milliseconds.
 */
qiran_status_t storage_nand_transfer_begin(void);
void           storage_nand_transfer_service(void);
bool           storage_nand_transfer_active(void);
bool           storage_nand_transfer_complete(void);

/* Only permitted once every stored record has been sent. */
qiran_status_t storage_nand_clear(void);

uint32_t storage_nand_used(void);
uint32_t storage_nand_capacity(void);
uint32_t storage_nand_records(void);
uint32_t storage_nand_transferred(void);
uint32_t storage_nand_write_failures(void);
uint32_t storage_nand_read_failures(void);
const char *storage_nand_type_name(nand_record_type_t type);

#endif
