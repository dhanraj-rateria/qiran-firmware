#ifndef QIRAN_COMM_CCSDS_H
#define QIRAN_COMM_CCSDS_H

#include "qiran/qiran_types.h"

/*
 * Space packet primary header, six octets, most significant octet first:
 *
 *   octet 0-1  version (3 bits), type (1), secondary header flag (1),
 *              application process identifier (11)
 *   octet 2-3  sequence flags (2), sequence count (14)
 *   octet 4-5  packet data length
 *
 * The length field holds one less than the octet count of the packet data
 * field, so a packet is six octets plus the field value plus one. That offset
 * is part of the standard and is applied in one place here rather than at each
 * call site.
 */
#define CCSDS_PRIMARY_HEADER_BYTES 6U

#define CCSDS_VERSION_1            0U
#define CCSDS_APID_MAX             0x7FFU
#define CCSDS_APID_IDLE            0x7FFU   /* reserved by the standard */
#define CCSDS_SEQ_COUNT_MAX        0x3FFFU
#define CCSDS_DATA_FIELD_MAX       65536U

typedef enum {
    CCSDS_TM = 0,   /* from the payload */
    CCSDS_TC = 1    /* to the payload */
} ccsds_type_t;

typedef enum {
    CCSDS_SEQ_CONTINUATION = 0,
    CCSDS_SEQ_FIRST        = 1,
    CCSDS_SEQ_LAST         = 2,
    CCSDS_SEQ_UNSEGMENTED  = 3
} ccsds_seq_flags_t;

typedef struct {
    uint8_t           version;
    ccsds_type_t      type;
    bool              secondary_header;
    uint16_t          apid;
    ccsds_seq_flags_t seq_flags;
    uint16_t          seq_count;
    uint16_t          data_length;   /* the field value, one less than the octets */
} ccsds_header_t;

/*
 * What a packet carries, which is what routing is by. The file class is present
 * and routable but deliberately unimplemented: file delivery is marked to be
 * confirmed in the requirements, and reserving its place here is what lets it
 * be added later without disturbing the router.
 */
typedef enum {
    CCSDS_CLASS_COMMAND = 0,
    CCSDS_CLASS_SCIENCE_RAW,
    CCSDS_CLASS_SCIENCE_PROCESSED,
    CCSDS_CLASS_HEALTH,
    CCSDS_CLASS_EVENT,
    CCSDS_CLASS_FILE,
    CCSDS_CLASS_COUNT
} ccsds_class_t;

typedef qiran_status_t (*ccsds_consumer_t)(void *ctx,
                                           const ccsds_header_t *header,
                                           const uint8_t *app_data,
                                           uint32_t app_len);

void comm_ccsds_init(void);

/*
 * The identifier assigned to each class. The values are an interface decision,
 * so they are data rather than constants and can be set from the interface
 * document without a rebuild.
 *
 * OPEN: confirm the identifier assignment.
 */
qiran_status_t comm_ccsds_set_apid(ccsds_class_t cls, uint16_t apid);
uint16_t       comm_ccsds_apid(ccsds_class_t cls);
ccsds_class_t  comm_ccsds_class_of(uint16_t apid);

qiran_status_t comm_ccsds_register(ccsds_class_t cls, ccsds_consumer_t consumer,
                                   void *ctx);

qiran_status_t comm_ccsds_build_header(const ccsds_header_t *header,
                                       uint8_t *buf, uint32_t len);
qiran_status_t comm_ccsds_parse_header(const uint8_t *buf, uint32_t len,
                                       ccsds_header_t *header);

/* Octets a whole packet occupies, given a parsed header. */
uint32_t comm_ccsds_packet_bytes(const ccsds_header_t *header);

/*
 * Builds a complete packet: header, application data, then a checksum over
 * that data. The sequence count for the class is applied and advanced here, so
 * a gap in it at the far end means packets were lost rather than mis-numbered.
 *
 * The checksum is a placeholder: the requirements leave the integrity mechanism
 * to the interface document and ask for a provisional one until it is fixed.
 */
#define CCSDS_INTEGRITY_BYTES 4U

qiran_status_t comm_ccsds_build(ccsds_class_t cls, const uint8_t *app_data,
                                uint32_t app_len, uint8_t *buf, uint32_t len,
                                uint32_t *written);

/*
 * Parses one packet, checks its declared length and its checksum, checks the
 * sequence count against what was expected for that identifier, and hands the
 * application data to the consumer registered for its class.
 */
qiran_status_t comm_ccsds_receive(const uint8_t *packet, uint32_t len);

uint32_t comm_ccsds_sent(ccsds_class_t cls);
uint32_t comm_ccsds_received(ccsds_class_t cls);
uint32_t comm_ccsds_sequence_gaps(void);
uint32_t comm_ccsds_integrity_errors(void);
uint32_t comm_ccsds_length_errors(void);
uint32_t comm_ccsds_unroutable(void);
const char *comm_ccsds_class_name(ccsds_class_t cls);

#endif
