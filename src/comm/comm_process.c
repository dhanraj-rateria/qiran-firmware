#include "qiran/comm/comm_process.h"

#include "qiran/comm/comm_ccsds.h"
#include "qiran/comm/comm_cmd.h"
#include "qiran/comm/comm_output.h"
#include "qiran/comm/comm_rs485_hk.h"
#include "qiran/comm/comm_spw_link.h"

/* A command arriving inside a space packet joins the same sequence as one
   arriving on the housekeeping link. */
static qiran_status_t consume_command(void *ctx, const ccsds_header_t *header,
                                      const uint8_t *app_data, uint32_t app_len)
{
    QIRAN_UNUSED(ctx);
    QIRAN_UNUSED(header);
    return comm_cmd_submit(app_data, app_len);
}

void comm_process_init(void)
{
    comm_rs485_hk_init();
    comm_cmd_init();
    comm_ccsds_init();
    comm_spw_link_init();
    comm_output_init();

    (void)comm_ccsds_register(CCSDS_CLASS_COMMAND, consume_command, NULL);
}

void communication_process(void)
{
    /*
     * Commands first, so one arriving this cycle is acted on before the frames
     * that report the resulting state go out. The link is serviced before the
     * queue that depends on it being up.
     */
    comm_cmd_service();
    comm_spw_link_service();
    comm_output_service();
    comm_rs485_hk_service();
}
