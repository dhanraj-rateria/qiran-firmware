#include "qiran/photonic/photonic_sched.h"

#include "qiran/plat/plat_cpu.h"
#include "qiran/svc/svc_fdir.h"

typedef struct {
    const char        *name;
    photonic_loop_fn_t service;
    void              *ctx;
    qiran_fault_id_t   fault;
    bool               registered;
    bool               enabled;

    uint32_t runs;
    uint32_t failures;
    uint32_t consecutive;
    uint32_t cycles_last;
    uint32_t cycles_worst;
} loop_t;

static loop_t s_loop[PHOTONIC_LOOP_COUNT];

static const char *const k_default_name[PHOTONIC_LOOP_COUNT] = {
    "laser_tec", "mrr", "crow", "umzi", "dli1", "dli2"
};

void photonic_sched_init(void)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)PHOTONIC_LOOP_COUNT; i++) {
        s_loop[i].name = k_default_name[i];
        s_loop[i].service = NULL;
        s_loop[i].ctx = NULL;
        s_loop[i].fault = QIRAN_FAULT_CONTROL_LOOP;
        s_loop[i].registered = false;
        s_loop[i].enabled = false;
        s_loop[i].runs = 0U;
        s_loop[i].failures = 0U;
        s_loop[i].consecutive = 0U;
        s_loop[i].cycles_last = 0U;
        s_loop[i].cycles_worst = 0U;
    }
}

qiran_status_t photonic_sched_register(photonic_loop_id_t id,
                                       const char *name,
                                       photonic_loop_fn_t service,
                                       void *ctx,
                                       qiran_fault_id_t fault)
{
    if ((id >= PHOTONIC_LOOP_COUNT) || (service == NULL) ||
        (fault >= QIRAN_FAULT_COUNT)) {
        return QIRAN_ERR_PARAM;
    }
    if (s_loop[id].registered) {
        return QIRAN_ERR_STATE;
    }

    if (name != NULL) {
        s_loop[id].name = name;
    }
    s_loop[id].service = service;
    s_loop[id].ctx = ctx;
    s_loop[id].fault = fault;
    s_loop[id].registered = true;

    return QIRAN_OK;
}

/* Enabling an unregistered loop is refused: it would report a lock as being
   held by a loop that does not exist. */
qiran_status_t photonic_sched_enable(photonic_loop_id_t id)
{
    if (id >= PHOTONIC_LOOP_COUNT) {
        return QIRAN_ERR_PARAM;
    }
    if (!s_loop[id].registered) {
        return QIRAN_ERR_UNSUPPORTED;
    }

    s_loop[id].enabled = true;
    s_loop[id].consecutive = 0U;
    return QIRAN_OK;
}

void photonic_sched_disable(photonic_loop_id_t id)
{
    if (id < PHOTONIC_LOOP_COUNT) {
        s_loop[id].enabled = false;
    }
}

void photonic_sched_disable_all(void)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)PHOTONIC_LOOP_COUNT; i++) {
        s_loop[i].enabled = false;
    }
}

bool photonic_sched_enabled(photonic_loop_id_t id)
{
    return (id < PHOTONIC_LOOP_COUNT) && s_loop[id].enabled;
}

void control_loop_service_calls(void)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)PHOTONIC_LOOP_COUNT; i++) {
        loop_t *l = &s_loop[i];
        uint32_t start;
        uint32_t elapsed;
        qiran_status_t st;

        if (!l->enabled) {
            continue;
        }

        start = plat_cpu_cycle_count();
        st = l->service(l->ctx);
        elapsed = plat_cpu_cycle_count() - start;

        l->runs++;
        l->cycles_last = elapsed;
        if (elapsed > l->cycles_worst) {
            l->cycles_worst = elapsed;
        }

        if (st == QIRAN_OK) {
            l->consecutive = 0U;
        } else {
            /*
             * Counted per occurrence and reported per occurrence, so the fault
             * class's own threshold decides when a persistently failing loop
             * escalates. A single missed iteration is not a fault worth acting
             * on; the same one every cycle is.
             */
            l->failures++;
            l->consecutive++;
            svc_fdir_report(l->fault, l->consecutive);
        }
    }
}

const char *photonic_loop_name(photonic_loop_id_t id)
{
    return (id < PHOTONIC_LOOP_COUNT) ? s_loop[id].name : "?";
}

void photonic_loop_stat_get(photonic_loop_id_t id, photonic_loop_stat_t *out)
{
    if ((id >= PHOTONIC_LOOP_COUNT) || (out == NULL)) {
        return;
    }

    out->runs = s_loop[id].runs;
    out->failures = s_loop[id].failures;
    out->consecutive_failures = s_loop[id].consecutive;
    out->cycles_last = s_loop[id].cycles_last;
    out->cycles_worst = s_loop[id].cycles_worst;
    out->enabled = s_loop[id].enabled;
    out->registered = s_loop[id].registered;
}

uint32_t photonic_sched_enabled_count(void)
{
    uint32_t i;
    uint32_t n = 0U;

    for (i = 0U; i < (uint32_t)PHOTONIC_LOOP_COUNT; i++) {
        if (s_loop[i].enabled) {
            n++;
        }
    }

    return n;
}
