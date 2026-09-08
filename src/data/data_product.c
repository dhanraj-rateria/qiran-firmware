#include "qiran/data/data_product.h"

#include "qiran/comm/comm_bytes.h"
#include "qiran/svc/svc_config.h"
#include "qiran/svc/svc_fdir.h"

static uint32_t s_computed;
static uint32_t s_inconsistent;

void data_product_init(void)
{
    s_computed = 0U;
    s_inconsistent = 0U;
}

static void clear(data_product_t *out)
{
    out->visibility = 0.0;
    out->s_bound = 0.0;
    out->pair_rate_hz = 0.0;
    out->brightness = 0.0;
    out->car_measured = 0.0;
    out->car_predicted = 0.0;
    out->car_lower_bound = 0.0;
    out->counts_valid = false;
    out->calibrated = false;
    out->consistent = false;
}

qiran_status_t data_product_compute(const data_product_input_t *in,
                                    data_product_t *out)
{
    const svc_calibration_t *cal;
    double sum;
    double integration_s;

    if ((in == NULL) || (out == NULL)) {
        return QIRAN_ERR_PARAM;
    }

    clear(out);

    sum = (double)in->coincidence_max + (double)in->coincidence_min;
    if ((sum <= 0.0) || (in->integration_ms == 0U)) {
        /*
         * No counts, or no interval over which to have counted them. Nothing
         * here is derivable, and returning zeros as though they were results
         * would put a visibility of zero into the record as a measurement.
         */
        return QIRAN_ERR_RANGE;
    }

    if (in->coincidence_max < in->coincidence_min) {
        return QIRAN_ERR_RANGE;
    }

    integration_s = (double)in->integration_ms / 1000.0;

    out->visibility = ((double)in->coincidence_max -
                       (double)in->coincidence_min) / sum;
    out->s_bound = DATA_S_COEFFICIENT * out->visibility;

    if (in->accidentals != 0U) {
        out->car_measured = (double)in->central_peak / (double)in->accidentals;
    }

    /*
     * The bound rises without limit as visibility approaches unity, so it is
     * only meaningful below it. At or above, the comparison it exists for
     * cannot be made and is not claimed.
     */
    if (out->visibility < 1.0) {
        out->car_lower_bound = out->visibility / (1.0 - out->visibility);
        out->consistent = out->car_lower_bound <= out->car_measured;
    }

    out->counts_valid = true;
    s_computed++;

    /*
     * The bound must sit below what was measured. That it does not is a finding
     * about the data rather than a failure of the payload, so it is logged at
     * the tier that means exactly that.
     */
    if ((in->accidentals != 0U) && (out->visibility < 1.0) &&
        !out->consistent) {
        s_inconsistent++;
        svc_fdir_report(QIRAN_FAULT_DATA_PRODUCT,
                        (uint32_t)(out->car_lower_bound * 1000.0));
    }

    cal = svc_config_calibration();
    if (cal == NULL) {
        return QIRAN_OK;
    }

    {
        double eta_signal = (double)cal->branch_efficiency_ppm[0] / 1000000.0;
        double eta_idler = (double)cal->branch_efficiency_ppm[1] / 1000000.0;
        double bandwidth_nm = (double)cal->spectral_bandwidth_pm / 1000.0;
        double pump_mw = (double)in->pump_power_uw / 1000.0;
        double coincidence_rate = (double)in->coincidence_max / integration_s;

        if ((eta_signal <= 0.0) || (eta_idler <= 0.0)) {
            return QIRAN_OK;
        }

        out->pair_rate_hz = coincidence_rate / (eta_signal * eta_idler);

        if ((pump_mw > 0.0) && (bandwidth_nm > 0.0)) {
            out->brightness = out->pair_rate_hz / (pump_mw * bandwidth_nm);
        }

        /*
         * The model value: true coincidences against those expected by chance
         * from the two singles rates within one coincidence window. Kept beside
         * the measured value as a check on the model, not as a replacement for
         * it.
         */
        if ((in->coincidence_window_ps != 0U) && (in->singles_signal != 0U) &&
            (in->singles_idler != 0U)) {
            double window_s = (double)in->coincidence_window_ps / 1e12;
            double singles_s = (double)in->singles_signal / integration_s;
            double singles_i = (double)in->singles_idler / integration_s;
            double accidental_rate = singles_s * singles_i * window_s;

            if (accidental_rate > 0.0) {
                out->car_predicted = coincidence_rate / accidental_rate;
            }
        }

        out->calibrated = true;
    }

    return QIRAN_OK;
}

/*
 * Rounded, not truncated. A ratio of ratios lands just under its exact value as
 * often as just over, and truncating would bias every telemetered figure
 * downward by up to one unit of its scale.
 *
 * Saturating at the top, so a value beyond the field is pinned rather than
 * wrapping into a small number that would read as a plausible result.
 */
static uint32_t scaled(double value, uint32_t scale)
{
    double v;

    if (value <= 0.0) {
        return 0U;
    }

    v = (value * (double)scale) + 0.5;
    if (v >= 4294967295.0) {
        return 0xFFFFFFFFU;
    }

    return (uint32_t)v;
}

qiran_status_t data_product_frame(const data_product_t *product, uint8_t *buf,
                                  uint32_t len, uint32_t *written)
{
    uint32_t at = 0U;
    uint8_t flags;

    if ((product == NULL) || (buf == NULL) ||
        (len < DATA_PRODUCT_FRAME_BYTES)) {
        return QIRAN_ERR_PARAM;
    }

    flags = (uint8_t)((product->counts_valid ? 0x01U : 0U) |
                      (product->calibrated ? 0x02U : 0U) |
                      (product->consistent ? 0x04U : 0U));

    at = comm_put_u32(buf, at, scaled(product->visibility,
                                      DATA_SCALE_VISIBILITY));
    at = comm_put_u32(buf, at, scaled(product->s_bound, DATA_SCALE_S_BOUND));
    at = comm_put_u32(buf, at, scaled(product->pair_rate_hz, 1U));
    at = comm_put_u32(buf, at, scaled(product->brightness,
                                      DATA_SCALE_BRIGHTNESS));
    at = comm_put_u32(buf, at, scaled(product->car_measured, DATA_SCALE_CAR));
    at = comm_put_u32(buf, at, scaled(product->car_predicted, DATA_SCALE_CAR));
    at = comm_put_u32(buf, at, scaled(product->car_lower_bound,
                                      DATA_SCALE_CAR));
    at = comm_put_u32(buf, at, (uint32_t)flags);
    at = comm_put_u32(buf, at, 0U);

    if (written != NULL) {
        *written = at;
    }

    return (at == DATA_PRODUCT_FRAME_BYTES) ? QIRAN_OK : QIRAN_ERR_INTEGRITY;
}

uint32_t data_product_computed(void)      { return s_computed; }
uint32_t data_product_inconsistent(void)  { return s_inconsistent; }
