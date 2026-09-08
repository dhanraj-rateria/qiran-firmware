#include "qiran/data/storage_nand.h"

#include "qiran/comm/comm_bytes.h"
#include "qiran/comm/comm_output.h"
#include "qiran/svc/svc_crc.h"
#include "qiran/svc/svc_fdir.h"
#include "qiran/svc/svc_log.h"

/*
 * Staging for one record on its way out. A record larger than this is stored
 * and counted but cannot be sent in one packet; segmenting it is a matter for
 * the interface document, which does not yet describe one.
 *
 * OPEN: how a product larger than one packet is to be segmented.
 */
#define NAND_TRANSFER_STAGE 512U

static storage_nand_ops_t s_ops;
static bool               s_ready;

static uint32_t s_used;
static uint32_t s_records;
static uint32_t s_sequence;

static bool     s_transfer_active;
static uint32_t s_transfer_offset;
static uint32_t s_transferred;

static uint32_t s_write_failures;
static uint32_t s_read_failures;

static uint8_t s_stage[NAND_TRANSFER_STAGE];

static const char *const k_type_name[NAND_RECORD_COUNT] = {
    "raw", "processed", "health"
};

void storage_nand_init(void)
{
    s_ops.erase = NULL;
    s_ops.write = NULL;
    s_ops.read = NULL;
    s_ops.capacity_bytes = 0U;
    s_ops.ctx = NULL;
    s_ready = false;

    s_used = 0U;
    s_records = 0U;
    s_sequence = 0U;
    s_transfer_active = false;
    s_transfer_offset = 0U;
    s_transferred = 0U;
    s_write_failures = 0U;
    s_read_failures = 0U;
}

qiran_status_t storage_nand_set_ops(const storage_nand_ops_t *ops)
{
    if (ops == NULL) {
        storage_nand_init();
        return QIRAN_OK;
    }

    if ((ops->erase == NULL) || (ops->write == NULL) || (ops->read == NULL) ||
        (ops->capacity_bytes == 0U)) {
        return QIRAN_ERR_PARAM;
    }

    s_ops = *ops;
    s_ready = true;
    s_used = 0U;
    s_records = 0U;
    return QIRAN_OK;
}

bool storage_nand_ready(void)
{
    return s_ready;
}

const char *storage_nand_type_name(nand_record_type_t type)
{
    return (type < NAND_RECORD_COUNT) ? k_type_name[type] : "?";
}

static ccsds_class_t class_of(nand_record_type_t type)
{
    switch (type) {
    case NAND_RECORD_PROCESSED: return CCSDS_CLASS_SCIENCE_PROCESSED;
    case NAND_RECORD_HEALTH:    return CCSDS_CLASS_HEALTH;
    case NAND_RECORD_RAW:
    default:                    return CCSDS_CLASS_SCIENCE_RAW;
    }
}

qiran_status_t storage_nand_store(nand_record_type_t type, const void *data,
                                  uint32_t len)
{
    uint8_t header[NAND_HEADER_BYTES];
    uint32_t at = 0U;

    if ((type >= NAND_RECORD_COUNT) || (data == NULL) || (len == 0U)) {
        return QIRAN_ERR_PARAM;
    }
    if (!s_ready) {
        return QIRAN_ERR_UNSUPPORTED;
    }
    if (s_transfer_active) {
        /* Appending during a transfer would move the end while it is read. */
        return QIRAN_ERR_BUSY;
    }
    if ((s_ops.capacity_bytes - s_used) < (NAND_HEADER_BYTES + len)) {
        svc_fdir_report(QIRAN_FAULT_NAND_WRITE, s_used);
        return QIRAN_ERR_RANGE;
    }

    at = comm_put_u32(header, at, NAND_RECORD_MAGIC);
    at = comm_put_u32(header, at, (uint32_t)type);
    at = comm_put_u32(header, at, s_sequence);
    at = comm_put_u32(header, at, len);
    (void)comm_put_u32(header, at, svc_crc32(data, len));

    if (s_ops.write(s_ops.ctx, s_used, header, NAND_HEADER_BYTES)
        != QIRAN_OK) {
        s_write_failures++;
        svc_fdir_report(QIRAN_FAULT_NAND_WRITE, s_used);
        return QIRAN_ERR_HARDWARE;
    }

    if (s_ops.write(s_ops.ctx, s_used + NAND_HEADER_BYTES, data, len)
        != QIRAN_OK) {
        s_write_failures++;
        svc_fdir_report(QIRAN_FAULT_NAND_WRITE, s_used);
        /*
         * The header is already down but its payload is not. The used mark is
         * left where it was, so the next store overwrites this partial record
         * rather than leaving it to be read back as a whole one.
         */
        return QIRAN_ERR_HARDWARE;
    }

    s_used += NAND_HEADER_BYTES + len;
    s_records++;
    s_sequence++;

    return QIRAN_OK;
}

qiran_status_t storage_nand_transfer_begin(void)
{
    if (!s_ready) {
        return QIRAN_ERR_UNSUPPORTED;
    }
    if (s_transfer_active) {
        return QIRAN_ERR_BUSY;
    }

    s_transfer_offset = 0U;
    s_transferred = 0U;
    s_transfer_active = true;
    return QIRAN_OK;
}

bool storage_nand_transfer_active(void)
{
    return s_transfer_active;
}

bool storage_nand_transfer_complete(void)
{
    return (!s_transfer_active) && (s_transferred == s_records) &&
           (s_records != 0U);
}

/*
 * One record per call. The queue it hands to refuses work when full, which is
 * what paces the transfer against how fast the link drains rather than against
 * how fast this can read.
 */
void storage_nand_transfer_service(void)
{
    uint8_t header[NAND_HEADER_BYTES];
    uint32_t magic;
    uint32_t type;
    uint32_t len;
    uint32_t crc;

    if (!s_transfer_active) {
        return;
    }

    if (s_transfer_offset >= s_used) {
        s_transfer_active = false;
        svc_log_event(0U, s_transferred);
        return;
    }

    if (s_ops.read(s_ops.ctx, s_transfer_offset, header, NAND_HEADER_BYTES)
        != QIRAN_OK) {
        s_read_failures++;
        svc_fdir_report(QIRAN_FAULT_NAND_WRITE, s_transfer_offset);
        s_transfer_active = false;
        return;
    }

    magic = comm_get_u32(header, 0U);
    type = comm_get_u32(header, 4U);
    len = comm_get_u32(header, 12U);
    crc = comm_get_u32(header, 16U);

    /*
     * A header that does not describe a record means the store is not what it
     * was written as, and reading on from a length taken out of damaged memory
     * would wander. The transfer stops here.
     */
    if ((magic != NAND_RECORD_MAGIC) || (type >= NAND_RECORD_COUNT) ||
        (len == 0U) ||
        ((s_transfer_offset + NAND_HEADER_BYTES + len) > s_used)) {
        s_read_failures++;
        svc_fdir_report(QIRAN_FAULT_NAND_WRITE, s_transfer_offset);
        s_transfer_active = false;
        return;
    }

    if (len > NAND_TRANSFER_STAGE) {
        /* Cannot be sent whole and must not be sent in part. Skipped and
           reported, so the rest of the store still gets out. */
        s_read_failures++;
        svc_fdir_report(QIRAN_FAULT_NAND_WRITE, len);
        s_transfer_offset += NAND_HEADER_BYTES + len;
        return;
    }

    if (s_ops.read(s_ops.ctx, s_transfer_offset + NAND_HEADER_BYTES, s_stage,
                   len) != QIRAN_OK) {
        s_read_failures++;
        svc_fdir_report(QIRAN_FAULT_NAND_WRITE, s_transfer_offset);
        s_transfer_active = false;
        return;
    }

    if (svc_crc32(s_stage, len) != crc) {
        /* This record has degraded. Reported and passed over: the ones after
           it are independent and still worth having. */
        s_read_failures++;
        svc_fdir_report(QIRAN_FAULT_PACKET_INTEGRITY, s_transfer_offset);
        s_transfer_offset += NAND_HEADER_BYTES + len;
        return;
    }

    if (comm_output_submit(class_of((nand_record_type_t)type), s_stage, len)
        != QIRAN_OK) {
        /* Queue full: try the same record again next cycle. */
        return;
    }

    s_transfer_offset += NAND_HEADER_BYTES + len;
    s_transferred++;
}

qiran_status_t storage_nand_clear(void)
{
    if (!s_ready) {
        return QIRAN_ERR_UNSUPPORTED;
    }
    if (s_transfer_active) {
        return QIRAN_ERR_BUSY;
    }
    /*
     * Refused while anything is still unsent. Clearing after a transfer that
     * stopped early would destroy the data that had not gone out yet.
     */
    if ((s_records != 0U) && (s_transferred != s_records)) {
        return QIRAN_ERR_STATE;
    }

    if (s_ops.erase(s_ops.ctx, 0U, s_used) != QIRAN_OK) {
        s_write_failures++;
        svc_fdir_report(QIRAN_FAULT_NAND_WRITE, 0U);
        return QIRAN_ERR_HARDWARE;
    }

    s_used = 0U;
    s_records = 0U;
    s_transferred = 0U;
    s_transfer_offset = 0U;
    return QIRAN_OK;
}

uint32_t storage_nand_used(void)           { return s_used; }
uint32_t storage_nand_capacity(void)       { return s_ops.capacity_bytes; }
uint32_t storage_nand_records(void)        { return s_records; }
uint32_t storage_nand_transferred(void)    { return s_transferred; }
uint32_t storage_nand_write_failures(void) { return s_write_failures; }
uint32_t storage_nand_read_failures(void)  { return s_read_failures; }
