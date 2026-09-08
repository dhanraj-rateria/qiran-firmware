#include "qiran/plat/plat_post.h"

#include "qiran/plat/plat_cpu.h"
#include "qiran/svc/svc_time.h"

typedef struct {
    const char      *name;
    plat_post_test_t test;
} post_entry_t;

static post_entry_t       s_entry[PLAT_POST_MAX];
static plat_post_result_t s_result[PLAT_POST_MAX];
static uint32_t           s_count;
static uint32_t           s_failures;

static volatile uint32_t *s_mem_base;
static uint32_t           s_mem_words;

static void flush(volatile uint32_t *base, uint32_t words)
{
    plat_cpu_dcache_flush_range(base, words * 4U);
}

static void invalidate(volatile uint32_t *base, uint32_t words)
{
    plat_cpu_dcache_invalidate_range(base, words * 4U);
}

/*
 * Address decode. Each power-of-two word offset is given a distinct value; a
 * shorted or open address line makes two offsets alias, which shows up as a
 * value written to one appearing at another.
 */
static qiran_status_t test_address_lines(volatile uint32_t *base, uint32_t words,
                                         uint32_t *fail_offset)
{
    const uint32_t base_pattern = 0xAAAAAAAAU;
    const uint32_t anti_pattern = 0x55555555U;
    uint32_t offset;

    for (offset = 1U; offset < words; offset <<= 1) {
        base[offset] = base_pattern ^ offset;
    }
    base[0] = anti_pattern;

    flush(base, words);
    invalidate(base, words);

    if (base[0] != anti_pattern) {
        *fail_offset = 0U;
        return QIRAN_ERR_HARDWARE;
    }

    for (offset = 1U; offset < words; offset <<= 1) {
        if (base[offset] != (base_pattern ^ offset)) {
            *fail_offset = offset;
            return QIRAN_ERR_HARDWARE;
        }
    }

    return QIRAN_OK;
}

/* Data path. Walking a single one through every bit position at one address
   catches a stuck, shorted or open data line. */
static qiran_status_t test_data_lines(volatile uint32_t *base, uint32_t *fail_offset)
{
    uint32_t bit;

    for (bit = 0U; bit < 32U; bit++) {
        uint32_t pattern = (uint32_t)1U << bit;

        base[0] = pattern;
        flush(base, 1U);
        invalidate(base, 1U);

        if (base[0] != pattern) {
            *fail_offset = bit;
            return QIRAN_ERR_HARDWARE;
        }
    }

    return QIRAN_OK;
}

/* Cell retention. Address-as-data then its complement covers every cell with
   both polarities and detects coupling between neighbours. */
static qiran_status_t test_cells(volatile uint32_t *base, uint32_t words,
                                 uint32_t *fail_offset)
{
    uint32_t i;
    uint32_t pass;

    for (pass = 0U; pass < 2U; pass++) {
        for (i = 0U; i < words; i++) {
            base[i] = (pass == 0U) ? i : ~i;
        }

        flush(base, words);
        invalidate(base, words);

        for (i = 0U; i < words; i++) {
            uint32_t expect = (pass == 0U) ? i : ~i;

            if (base[i] != expect) {
                *fail_offset = i;
                return QIRAN_ERR_HARDWARE;
            }
        }
    }

    return QIRAN_OK;
}

qiran_status_t plat_post_memory_test(volatile uint32_t *base, uint32_t words,
                                     uint32_t *fail_offset)
{
    qiran_status_t st;
    uint32_t scratch = 0U;
    uint32_t *out = (fail_offset != NULL) ? fail_offset : &scratch;

    if ((base == NULL) || (words < 2U)) {
        return QIRAN_ERR_PARAM;
    }

    *out = 0U;

    st = test_data_lines(base, out);
    if (st != QIRAN_OK) {
        return st;
    }

    st = test_address_lines(base, words, out);
    if (st != QIRAN_OK) {
        return st;
    }

    return test_cells(base, words, out);
}

static qiran_status_t builtin_memory_test(uint32_t *detail)
{
    if (s_mem_base == NULL) {
        return QIRAN_ERR_UNSUPPORTED;
    }
    return plat_post_memory_test(s_mem_base, s_mem_words, detail);
}

void plat_post_init(void)
{
    s_count = 0U;
    s_failures = 0U;
    s_mem_base = NULL;
    s_mem_words = 0U;

    (void)plat_post_register("memory", builtin_memory_test);
}

qiran_status_t plat_post_register(const char *name, plat_post_test_t test)
{
    if ((name == NULL) || (test == NULL)) {
        return QIRAN_ERR_PARAM;
    }
    if (s_count >= PLAT_POST_MAX) {
        return QIRAN_ERR_RANGE;
    }

    s_entry[s_count].name = name;
    s_entry[s_count].test = test;
    s_count++;

    return QIRAN_OK;
}

qiran_status_t plat_post_set_memory_region(void *base, uint32_t bytes)
{
    if ((base == NULL) || (bytes < 8U)) {
        return QIRAN_ERR_PARAM;
    }

    s_mem_base = (volatile uint32_t *)base;
    s_mem_words = bytes / 4U;

    return QIRAN_OK;
}

/*
 * Every test runs even after one fails, so a single failure yields the whole
 * picture rather than only the first symptom.
 */
qiran_status_t plat_post_run(void)
{
    qiran_status_t overall = QIRAN_OK;
    uint32_t i;

    s_failures = 0U;

    for (i = 0U; i < s_count; i++) {
        uint64_t started = svc_time_now_ms();
        uint32_t detail = 0U;
        qiran_status_t st = s_entry[i].test(&detail);

        s_result[i].name = s_entry[i].name;
        s_result[i].result = st;
        s_result[i].detail = detail;
        s_result[i].duration_ms = (uint32_t)(svc_time_now_ms() - started);
        s_result[i].ran = (st != QIRAN_ERR_UNSUPPORTED);

        if (s_result[i].ran && (st != QIRAN_OK)) {
            s_failures++;
            overall = QIRAN_ERR_HARDWARE;
        }
    }

    return overall;
}

uint32_t plat_post_count(void)    { return s_count; }
uint32_t plat_post_failures(void) { return s_failures; }

bool plat_post_result_get(uint32_t index, plat_post_result_t *out)
{
    if ((index >= s_count) || (out == NULL)) {
        return false;
    }
    *out = s_result[index];
    return true;
}
