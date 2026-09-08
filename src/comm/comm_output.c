#include "qiran/comm/comm_output.h"

#include "qiran/comm/comm_spw_link.h"
#include "qiran/svc/svc_fdir.h"

/*
 * Lower is sent sooner. Fault and event data first because it explains
 * everything else; status next; processed results before raw, since a run's
 * conclusions are worth more than its unreduced samples if only some of it
 * gets through. File delivery sits with raw as bulk.
 */
static const uint8_t k_priority[CCSDS_CLASS_COUNT] = {
    3U,  /* command: not an outbound class, ranked mid so it is never first */
    4U,  /* raw science */
    2U,  /* processed science */
    1U,  /* health */
    0U,  /* event and fault */
    4U   /* file delivery */
};

QIRAN_STATIC_ASSERT(QIRAN_ARRAY_LEN(k_priority) == (size_t)CCSDS_CLASS_COUNT,
                    every_class_has_a_priority);

/* One packet's worth of staging, reused: only one packet is in flight at once. */
#define OUTPUT_PACKET_BYTES 1024U

typedef struct {
    const uint8_t *data;
    uint32_t       len;
    ccsds_class_t  cls;
    bool           used;
} slot_t;

static slot_t   s_slot[COMM_OUTPUT_DEPTH];
static uint32_t s_pending;
static uint32_t s_high_water;
static uint32_t s_submitted;
static uint32_t s_sent;
static uint32_t s_rejected;
static uint32_t s_failed;

static uint8_t s_packet[OUTPUT_PACKET_BYTES];

void comm_output_init(void)
{
    uint32_t i;

    for (i = 0U; i < COMM_OUTPUT_DEPTH; i++) {
        s_slot[i].used = false;
    }

    s_pending = 0U;
    s_high_water = 0U;
    s_submitted = 0U;
    s_sent = 0U;
    s_rejected = 0U;
    s_failed = 0U;
}

uint8_t comm_output_priority_of(ccsds_class_t cls)
{
    return (cls < CCSDS_CLASS_COUNT) ? k_priority[cls] : 0xFFU;
}

qiran_status_t comm_output_submit(ccsds_class_t cls, const uint8_t *data,
                                  uint32_t len)
{
    uint32_t i;

    if ((cls >= CCSDS_CLASS_COUNT) || (data == NULL) || (len == 0U)) {
        return QIRAN_ERR_PARAM;
    }
    if ((len + CCSDS_PRIMARY_HEADER_BYTES + CCSDS_INTEGRITY_BYTES) >
        OUTPUT_PACKET_BYTES) {
        return QIRAN_ERR_RANGE;
    }

    for (i = 0U; i < COMM_OUTPUT_DEPTH; i++) {
        if (s_slot[i].used) {
            continue;
        }

        s_slot[i].data = data;
        s_slot[i].len = len;
        s_slot[i].cls = cls;
        s_slot[i].used = true;

        s_pending++;
        s_submitted++;
        if (s_pending > s_high_water) {
            s_high_water = s_pending;
        }
        return QIRAN_OK;
    }

    s_rejected++;
    svc_fdir_report(QIRAN_FAULT_OUTPUT_QUEUE, (uint32_t)cls);
    return QIRAN_ERR_BUSY;
}

/*
 * Earliest submission wins among equal priorities, which is why the scan keeps
 * the lowest index at the best priority rather than the last one found.
 */
static uint32_t pick(void)
{
    uint32_t best = COMM_OUTPUT_DEPTH;
    uint8_t best_priority = 0xFFU;
    uint32_t i;

    for (i = 0U; i < COMM_OUTPUT_DEPTH; i++) {
        if (!s_slot[i].used) {
            continue;
        }
        if (k_priority[s_slot[i].cls] < best_priority) {
            best_priority = k_priority[s_slot[i].cls];
            best = i;
        }
    }

    return best;
}

void comm_output_service(void)
{
    while (comm_spw_link_running()) {
        uint32_t index = pick();
        uint32_t written = 0U;
        slot_t *slot;

        if (index >= COMM_OUTPUT_DEPTH) {
            return;
        }

        slot = &s_slot[index];

        if (comm_ccsds_build(slot->cls, slot->data, slot->len, s_packet,
                             (uint32_t)sizeof(s_packet), &written) != QIRAN_OK) {
            /*
             * Unpacketisable, so retrying would fail identically. Dropped and
             * reported rather than blocking everything behind it.
             */
            slot->used = false;
            s_pending--;
            s_failed++;
            svc_fdir_report(QIRAN_FAULT_OUTPUT_QUEUE, (uint32_t)slot->cls);
            continue;
        }

        if (comm_spw_link_send(s_packet, written) != QIRAN_OK) {
            /* Left queued: the link may take it on a later pass. */
            return;
        }

        slot->used = false;
        s_pending--;
        s_sent++;
    }
}

uint32_t comm_output_pending(void)    { return s_pending; }
uint32_t comm_output_high_water(void) { return s_high_water; }
uint32_t comm_output_submitted(void)  { return s_submitted; }
uint32_t comm_output_sent(void)       { return s_sent; }
uint32_t comm_output_rejected(void)   { return s_rejected; }
uint32_t comm_output_failed(void)     { return s_failed; }
