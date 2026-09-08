#include "qiran/comm/comm_ccsds.h"

#include "qiran/comm/comm_bytes.h"
#include "qiran/svc/svc_crc.h"
#include "qiran/svc/svc_fdir.h"

/*
 * Provisional identifier assignment, grouped so that a class is recognisable
 * from the value while the real assignment is pending.
 */
static const uint16_t k_default_apid[CCSDS_CLASS_COUNT] = {
    0x001U,  /* command */
    0x010U,  /* raw science */
    0x011U,  /* processed science */
    0x020U,  /* health */
    0x030U,  /* event and fault */
    0x040U   /* file delivery, reserved */
};

static const char *const k_class_name[CCSDS_CLASS_COUNT] = {
    "command", "raw", "processed", "health", "event", "file"
};

static uint16_t         s_apid[CCSDS_CLASS_COUNT];
static ccsds_consumer_t s_consumer[CCSDS_CLASS_COUNT];
static void            *s_ctx[CCSDS_CLASS_COUNT];

static uint16_t s_tx_count[CCSDS_CLASS_COUNT];
static bool     s_rx_seen[CCSDS_CLASS_COUNT];
static uint16_t s_rx_expect[CCSDS_CLASS_COUNT];
static uint32_t s_sent[CCSDS_CLASS_COUNT];
static uint32_t s_received[CCSDS_CLASS_COUNT];

static uint32_t s_gaps;
static uint32_t s_integrity_errors;
static uint32_t s_length_errors;
static uint32_t s_unroutable;

void comm_ccsds_init(void)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)CCSDS_CLASS_COUNT; i++) {
        s_apid[i] = k_default_apid[i];
        s_consumer[i] = NULL;
        s_ctx[i] = NULL;
        s_tx_count[i] = 0U;
        s_rx_seen[i] = false;
        s_rx_expect[i] = 0U;
        s_sent[i] = 0U;
        s_received[i] = 0U;
    }

    s_gaps = 0U;
    s_integrity_errors = 0U;
    s_length_errors = 0U;
    s_unroutable = 0U;
}

qiran_status_t comm_ccsds_set_apid(ccsds_class_t cls, uint16_t apid)
{
    uint32_t i;

    if ((cls >= CCSDS_CLASS_COUNT) || (apid > CCSDS_APID_MAX) ||
        (apid == CCSDS_APID_IDLE)) {
        return QIRAN_ERR_PARAM;
    }

    /* Two classes sharing an identifier would make routing ambiguous. */
    for (i = 0U; i < (uint32_t)CCSDS_CLASS_COUNT; i++) {
        if ((i != (uint32_t)cls) && (s_apid[i] == apid)) {
            return QIRAN_ERR_STATE;
        }
    }

    s_apid[cls] = apid;
    return QIRAN_OK;
}

uint16_t comm_ccsds_apid(ccsds_class_t cls)
{
    return (cls < CCSDS_CLASS_COUNT) ? s_apid[cls] : CCSDS_APID_IDLE;
}

ccsds_class_t comm_ccsds_class_of(uint16_t apid)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)CCSDS_CLASS_COUNT; i++) {
        if (s_apid[i] == apid) {
            return (ccsds_class_t)i;
        }
    }

    return CCSDS_CLASS_COUNT;
}

qiran_status_t comm_ccsds_register(ccsds_class_t cls, ccsds_consumer_t consumer,
                                   void *ctx)
{
    if ((cls >= CCSDS_CLASS_COUNT) || (consumer == NULL)) {
        return QIRAN_ERR_PARAM;
    }
    if (s_consumer[cls] != NULL) {
        return QIRAN_ERR_STATE;
    }

    s_consumer[cls] = consumer;
    s_ctx[cls] = ctx;
    return QIRAN_OK;
}

const char *comm_ccsds_class_name(ccsds_class_t cls)
{
    return (cls < CCSDS_CLASS_COUNT) ? k_class_name[cls] : "?";
}

qiran_status_t comm_ccsds_build_header(const ccsds_header_t *header,
                                       uint8_t *buf, uint32_t len)
{
    uint16_t word0;
    uint16_t word1;
    uint32_t at = 0U;

    if ((header == NULL) || (buf == NULL) || (len < CCSDS_PRIMARY_HEADER_BYTES)) {
        return QIRAN_ERR_PARAM;
    }
    if ((header->version > 7U) || (header->apid > CCSDS_APID_MAX) ||
        (header->seq_count > CCSDS_SEQ_COUNT_MAX)) {
        return QIRAN_ERR_RANGE;
    }

    word0 = (uint16_t)(((uint16_t)(header->version & 0x07U) << 13) |
                       ((uint16_t)((uint16_t)header->type & 0x01U) << 12) |
                       ((uint16_t)(header->secondary_header ? 1U : 0U) << 11) |
                       (header->apid & CCSDS_APID_MAX));

    word1 = (uint16_t)(((uint16_t)((uint16_t)header->seq_flags & 0x03U) << 14) |
                       (header->seq_count & CCSDS_SEQ_COUNT_MAX));

    at = comm_put_u16(buf, at, word0);
    at = comm_put_u16(buf, at, word1);
    (void)comm_put_u16(buf, at, header->data_length);

    return QIRAN_OK;
}

qiran_status_t comm_ccsds_parse_header(const uint8_t *buf, uint32_t len,
                                       ccsds_header_t *header)
{
    uint16_t word0;
    uint16_t word1;

    if ((buf == NULL) || (header == NULL) ||
        (len < CCSDS_PRIMARY_HEADER_BYTES)) {
        return QIRAN_ERR_PARAM;
    }

    word0 = comm_get_u16(buf, 0U);
    word1 = comm_get_u16(buf, 2U);

    header->version = (uint8_t)((word0 >> 13) & 0x07U);
    header->type = (ccsds_type_t)((word0 >> 12) & 0x01U);
    header->secondary_header = ((word0 >> 11) & 0x01U) != 0U;
    header->apid = (uint16_t)(word0 & CCSDS_APID_MAX);
    header->seq_flags = (ccsds_seq_flags_t)((word1 >> 14) & 0x03U);
    header->seq_count = (uint16_t)(word1 & CCSDS_SEQ_COUNT_MAX);
    header->data_length = comm_get_u16(buf, 4U);

    return QIRAN_OK;
}

uint32_t comm_ccsds_packet_bytes(const ccsds_header_t *header)
{
    if (header == NULL) {
        return 0U;
    }
    return CCSDS_PRIMARY_HEADER_BYTES + (uint32_t)header->data_length + 1U;
}

qiran_status_t comm_ccsds_build(ccsds_class_t cls, const uint8_t *app_data,
                                uint32_t app_len, uint8_t *buf, uint32_t len,
                                uint32_t *written)
{
    ccsds_header_t header;
    uint32_t field_len;
    uint32_t total;
    uint32_t i;
    qiran_status_t st;

    if ((cls >= CCSDS_CLASS_COUNT) || (buf == NULL) ||
        ((app_data == NULL) && (app_len != 0U))) {
        return QIRAN_ERR_PARAM;
    }

    field_len = app_len + CCSDS_INTEGRITY_BYTES;
    total = CCSDS_PRIMARY_HEADER_BYTES + field_len;

    if ((field_len == 0U) || (field_len > CCSDS_DATA_FIELD_MAX)) {
        return QIRAN_ERR_RANGE;
    }
    if (len < total) {
        return QIRAN_ERR_RANGE;
    }

    header.version = CCSDS_VERSION_1;
    header.type = CCSDS_TM;
    header.secondary_header = false;
    header.apid = s_apid[cls];
    header.seq_flags = CCSDS_SEQ_UNSEGMENTED;
    header.seq_count = s_tx_count[cls];
    header.data_length = (uint16_t)(field_len - 1U);

    st = comm_ccsds_build_header(&header, buf, len);
    if (st != QIRAN_OK) {
        return st;
    }

    for (i = 0U; i < app_len; i++) {
        buf[CCSDS_PRIMARY_HEADER_BYTES + i] = app_data[i];
    }

    (void)comm_put_u32(buf, CCSDS_PRIMARY_HEADER_BYTES + app_len,
                       svc_crc32(&buf[CCSDS_PRIMARY_HEADER_BYTES], app_len));

    /* Wraps within its fourteen bits, as the standard intends. */
    s_tx_count[cls] = (uint16_t)((s_tx_count[cls] + 1U) & CCSDS_SEQ_COUNT_MAX);
    s_sent[cls]++;

    if (written != NULL) {
        *written = total;
    }

    return QIRAN_OK;
}

qiran_status_t comm_ccsds_receive(const uint8_t *packet, uint32_t len)
{
    ccsds_header_t header;
    ccsds_class_t cls;
    uint32_t field_len;
    uint32_t app_len;
    const uint8_t *app;
    uint32_t stored;
    qiran_status_t st;

    st = comm_ccsds_parse_header(packet, len, &header);
    if (st != QIRAN_OK) {
        return st;
    }

    field_len = (uint32_t)header.data_length + 1U;

    /*
     * The declared length must match what actually arrived. A packet shorter
     * than it claims would otherwise have the checksum read past its end.
     */
    if ((CCSDS_PRIMARY_HEADER_BYTES + field_len) != len) {
        s_length_errors++;
        svc_fdir_report(QIRAN_FAULT_PACKET_INTEGRITY, field_len);
        return QIRAN_ERR_INTEGRITY;
    }

    if (field_len < CCSDS_INTEGRITY_BYTES) {
        s_length_errors++;
        svc_fdir_report(QIRAN_FAULT_PACKET_INTEGRITY, field_len);
        return QIRAN_ERR_INTEGRITY;
    }

    app = &packet[CCSDS_PRIMARY_HEADER_BYTES];
    app_len = field_len - CCSDS_INTEGRITY_BYTES;

    stored = comm_get_u32(packet, CCSDS_PRIMARY_HEADER_BYTES + app_len);
    if (stored != svc_crc32(app, app_len)) {
        s_integrity_errors++;
        svc_fdir_report(QIRAN_FAULT_PACKET_INTEGRITY, header.apid);
        return QIRAN_ERR_INTEGRITY;
    }

    cls = comm_ccsds_class_of(header.apid);
    if (cls >= CCSDS_CLASS_COUNT) {
        s_unroutable++;
        svc_fdir_report(QIRAN_FAULT_PACKET_INTEGRITY, header.apid);
        return QIRAN_ERR_UNSUPPORTED;
    }

    /*
     * Checked after the checksum, so a corrupted count is not mistaken for a
     * lost packet. A gap is reported and then adopted: continuing to expect a
     * count that will never arrive would report a gap on every later packet.
     */
    if (s_rx_seen[cls] && (header.seq_count != s_rx_expect[cls])) {
        s_gaps++;
        svc_fdir_report(QIRAN_FAULT_PACKET_SEQUENCE,
                        ((uint32_t)s_rx_expect[cls] << 16) |
                        (uint32_t)header.seq_count);
    }

    s_rx_seen[cls] = true;
    s_rx_expect[cls] = (uint16_t)((header.seq_count + 1U) & CCSDS_SEQ_COUNT_MAX);
    s_received[cls]++;

    if (s_consumer[cls] == NULL) {
        s_unroutable++;
        return QIRAN_ERR_UNSUPPORTED;
    }

    return s_consumer[cls](s_ctx[cls], &header, app, app_len);
}

uint32_t comm_ccsds_sent(ccsds_class_t cls)
{
    return (cls < CCSDS_CLASS_COUNT) ? s_sent[cls] : 0U;
}

uint32_t comm_ccsds_received(ccsds_class_t cls)
{
    return (cls < CCSDS_CLASS_COUNT) ? s_received[cls] : 0U;
}

uint32_t comm_ccsds_sequence_gaps(void)      { return s_gaps; }
uint32_t comm_ccsds_integrity_errors(void)   { return s_integrity_errors; }
uint32_t comm_ccsds_length_errors(void)      { return s_length_errors; }
uint32_t comm_ccsds_unroutable(void)         { return s_unroutable; }
