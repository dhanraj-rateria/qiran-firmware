#ifndef QIRAN_DATA_PRODUCT_H
#define QIRAN_DATA_PRODUCT_H

#include "qiran/qiran_types.h"

/*
 * The derived quantities computed from one run's counts.
 *
 * These are computed in floating point, unlike the configuration store which
 * is integer throughout. The reason is range rather than taste: a pair rate and
 * a brightness span several orders of magnitude and are formed from ratios of
 * ratios, where fixed point would need a different scale at each step and every
 * one of them would be a place to get it wrong. None of this runs in interrupt
 * context, so the rule that keeps floating point out of there is untouched.
 *
 * What goes on the wire is scaled integers, so the transmitted form does not
 * depend on how the processor represents a double.
 */

/* Twice the square root of two, the coefficient relating the correlation
   bound to visibility. Stated rather than computed, to avoid needing a maths
   library on the target for one constant. */
#define DATA_S_COEFFICIENT 2.8284271247461903

typedef struct {
    uint32_t coincidence_max;     /* highest coincidence count over the scan */
    uint32_t coincidence_min;     /* lowest */
    uint32_t singles_signal;
    uint32_t singles_idler;
    uint32_t central_peak;        /* histogram peak above the accidentals floor */
    uint32_t accidentals;         /* equal-width window at far delay */
    uint32_t integration_ms;
    uint32_t pump_power_uw;
    uint32_t coincidence_window_ps;
} data_product_input_t;

typedef struct {
    double visibility;
    double s_bound;
    double pair_rate_hz;
    double brightness;
    double car_measured;
    double car_predicted;
    double car_lower_bound;

    /* Computable from the counts alone. */
    bool counts_valid;
    /* Needed ground calibration, and it was available and usable. */
    bool calibrated;
    /* The bound from visibility sits below the measured value, as it must. */
    bool consistent;
} data_product_t;

void data_product_init(void);

/*
 * Computes everything derivable from the input. Quantities needing ground
 * calibration are left out, with `calibrated` false, when the calibration is
 * absent or unusable, rather than the whole product being discarded: the
 * visibility and the correlation bound are the run's primary results and do
 * not depend on it.
 */
qiran_status_t data_product_compute(const data_product_input_t *in,
                                    data_product_t *out);

/*
 * Scale factors applied when framing, so the wire carries integers.
 */
#define DATA_SCALE_VISIBILITY 1000000U
#define DATA_SCALE_S_BOUND    1000000U
#define DATA_SCALE_CAR        1000U
#define DATA_SCALE_BRIGHTNESS 1000U

#define DATA_PRODUCT_FRAME_BYTES 36U

qiran_status_t data_product_frame(const data_product_t *product, uint8_t *buf,
                                  uint32_t len, uint32_t *written);

uint32_t data_product_computed(void);
uint32_t data_product_inconsistent(void);

#endif
