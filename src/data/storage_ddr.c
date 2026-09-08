#include "qiran/data/storage_ddr.h"

#include "qiran/svc/svc_fdir.h"

typedef struct {
    uint8_t    *base;
    uint32_t    capacity;
    uint32_t    used;
    ddr_owner_t owner;
} region_t;

static region_t s_region[DDR_REGION_COUNT];
static uint32_t s_violations;
static uint32_t s_handoffs;
static uint32_t s_overruns;

static const char *const k_owner_name[DDR_OWNER_COUNT] = {
    "none", "pl-dma", "ps-processing", "storage", "comms"
};

static const char *const k_region_name[DDR_REGION_COUNT] = {
    "raw0a", "raw0b", "raw1a", "raw1b", "processed", "tx", "scratch"
};

void storage_ddr_init(void)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)DDR_REGION_COUNT; i++) {
        s_region[i].base = NULL;
        s_region[i].capacity = 0U;
        s_region[i].used = 0U;
        s_region[i].owner = DDR_OWNER_NONE;
    }

    s_violations = 0U;
    s_handoffs = 0U;
    s_overruns = 0U;
}

qiran_status_t storage_ddr_set_region(ddr_region_t region, void *base,
                                      uint32_t bytes)
{
    if ((region >= DDR_REGION_COUNT) || (base == NULL) || (bytes == 0U)) {
        return QIRAN_ERR_PARAM;
    }

    /* Changing a region under its owner would move memory out from under it. */
    if (s_region[region].owner != DDR_OWNER_NONE) {
        return QIRAN_ERR_STATE;
    }

    s_region[region].base = (uint8_t *)base;
    s_region[region].capacity = bytes;
    s_region[region].used = 0U;

    return QIRAN_OK;
}

bool storage_ddr_configured(ddr_region_t region)
{
    return (region < DDR_REGION_COUNT) && (s_region[region].base != NULL);
}

ddr_owner_t storage_ddr_owner(ddr_region_t region)
{
    return (region < DDR_REGION_COUNT) ? s_region[region].owner
                                       : DDR_OWNER_NONE;
}

const char *storage_ddr_owner_name(ddr_owner_t owner)
{
    return (owner < DDR_OWNER_COUNT) ? k_owner_name[owner] : "?";
}

const char *storage_ddr_region_name(ddr_region_t region)
{
    return (region < DDR_REGION_COUNT) ? k_region_name[region] : "?";
}

static void violation(ddr_region_t region, ddr_owner_t expected)
{
    s_violations++;
    svc_fdir_report(QIRAN_FAULT_BUFFER_OWNERSHIP,
                    ((uint32_t)region << 16) | ((uint32_t)expected << 8) |
                    (uint32_t)s_region[region].owner);
}

qiran_status_t storage_ddr_handoff(ddr_region_t region, ddr_owner_t from,
                                   ddr_owner_t to)
{
    if ((region >= DDR_REGION_COUNT) || (from >= DDR_OWNER_COUNT) ||
        (to >= DDR_OWNER_COUNT)) {
        return QIRAN_ERR_PARAM;
    }
    if (s_region[region].base == NULL) {
        return QIRAN_ERR_UNSUPPORTED;
    }

    if (s_region[region].owner != from) {
        violation(region, from);
        return QIRAN_ERR_STATE;
    }

    s_region[region].owner = to;
    s_handoffs++;
    return QIRAN_OK;
}

qiran_status_t storage_ddr_clear(ddr_region_t region, ddr_owner_t from)
{
    if ((region >= DDR_REGION_COUNT) || (from >= DDR_OWNER_COUNT)) {
        return QIRAN_ERR_PARAM;
    }
    if (s_region[region].owner != from) {
        violation(region, from);
        return QIRAN_ERR_STATE;
    }

    s_region[region].used = 0U;
    s_region[region].owner = DDR_OWNER_NONE;
    return QIRAN_OK;
}

qiran_status_t storage_ddr_append(ddr_region_t region, ddr_owner_t owner,
                                  const void *data, uint32_t len)
{
    region_t *r;
    const uint8_t *src = (const uint8_t *)data;
    uint32_t i;

    if ((region >= DDR_REGION_COUNT) || (data == NULL) || (len == 0U)) {
        return QIRAN_ERR_PARAM;
    }

    r = &s_region[region];

    if (r->base == NULL) {
        return QIRAN_ERR_UNSUPPORTED;
    }
    if (r->owner != owner) {
        violation(region, owner);
        return QIRAN_ERR_STATE;
    }

    if ((r->capacity - r->used) < len) {
        /*
         * Refused rather than truncated. A partial record looks like a whole
         * one to whatever reads it back, which is worse than not having it.
         */
        s_overruns++;
        svc_fdir_report(QIRAN_FAULT_DDR_OVERRUN, (uint32_t)region);
        return QIRAN_ERR_RANGE;
    }

    for (i = 0U; i < len; i++) {
        r->base[r->used + i] = src[i];
    }
    r->used += len;

    return QIRAN_OK;
}

qiran_status_t storage_ddr_set_used(ddr_region_t region, ddr_owner_t owner,
                                    uint32_t used)
{
    region_t *r;

    if (region >= DDR_REGION_COUNT) {
        return QIRAN_ERR_PARAM;
    }

    r = &s_region[region];

    if (r->base == NULL) {
        return QIRAN_ERR_UNSUPPORTED;
    }
    if (r->owner != owner) {
        violation(region, owner);
        return QIRAN_ERR_STATE;
    }
    if (used > r->capacity) {
        s_overruns++;
        svc_fdir_report(QIRAN_FAULT_DDR_OVERRUN, (uint32_t)region);
        return QIRAN_ERR_RANGE;
    }

    r->used = used;
    return QIRAN_OK;
}

qiran_status_t storage_ddr_read(ddr_region_t region, ddr_owner_t owner,
                                const uint8_t **data, uint32_t *len)
{
    region_t *r;

    if ((region >= DDR_REGION_COUNT) || (data == NULL) || (len == NULL)) {
        return QIRAN_ERR_PARAM;
    }

    r = &s_region[region];

    if (r->base == NULL) {
        return QIRAN_ERR_UNSUPPORTED;
    }
    if (r->owner != owner) {
        violation(region, owner);
        return QIRAN_ERR_STATE;
    }

    *data = r->base;
    *len = r->used;
    return QIRAN_OK;
}

uint32_t storage_ddr_used(ddr_region_t region)
{
    return (region < DDR_REGION_COUNT) ? s_region[region].used : 0U;
}

uint32_t storage_ddr_capacity(ddr_region_t region)
{
    return (region < DDR_REGION_COUNT) ? s_region[region].capacity : 0U;
}

uint32_t storage_ddr_free(ddr_region_t region)
{
    if (region >= DDR_REGION_COUNT) {
        return 0U;
    }
    return s_region[region].capacity - s_region[region].used;
}

ddr_region_t storage_ddr_free_raw_slot(uint32_t channel)
{
    ddr_region_t first;
    uint32_t i;

    if (channel >= DDR_CHANNELS) {
        return DDR_REGION_COUNT;
    }

    first = (ddr_region_t)(channel * DDR_RAW_SLOTS);

    for (i = 0U; i < DDR_RAW_SLOTS; i++) {
        ddr_region_t region = (ddr_region_t)((uint32_t)first + i);

        if ((s_region[region].base != NULL) &&
            (s_region[region].owner == DDR_OWNER_NONE)) {
            return region;
        }
    }

    return DDR_REGION_COUNT;
}

uint32_t storage_ddr_violations(void) { return s_violations; }
uint32_t storage_ddr_handoffs(void)   { return s_handoffs; }
uint32_t storage_ddr_overruns(void)   { return s_overruns; }
