#ifndef QIRAN_PLAT_POST_H
#define QIRAN_PLAT_POST_H

#include "qiran/qiran_types.h"

#define PLAT_POST_MAX 16U

typedef qiran_status_t (*plat_post_test_t)(uint32_t *detail);

typedef struct {
    const char    *name;
    qiran_status_t result;
    uint32_t       detail;
    uint32_t       duration_ms;
    bool           ran;
} plat_post_result_t;

void plat_post_init(void);

/* Subsystem owners register their own self test; this module owns only the
   memory test, which it can perform without any device driver. */
qiran_status_t plat_post_register(const char *name, plat_post_test_t test);

/*
 * Region used by the built-in memory test. It must not overlap code, stack or
 * any live data, since the test destroys its contents. With no region set the
 * memory test is recorded as not run rather than as passed.
 */
qiran_status_t plat_post_set_memory_region(void *base, uint32_t bytes);

qiran_status_t plat_post_run(void);

uint32_t plat_post_count(void);
bool     plat_post_result_get(uint32_t index, plat_post_result_t *out);
uint32_t plat_post_failures(void);

/*
 * Memory test against three fault models: address decode, data path, and
 * individual cell retention. Exposed so it can be exercised over any region.
 * Destroys the contents of the region. Returns the offending word offset in
 * *fail_offset when it fails.
 */
qiran_status_t plat_post_memory_test(volatile uint32_t *base,
                                     uint32_t words,
                                     uint32_t *fail_offset);

#endif
