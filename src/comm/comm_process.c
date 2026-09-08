#include "qiran/comm/comm_process.h"

#include "qiran/comm/comm_cmd.h"
#include "qiran/comm/comm_rs485_hk.h"

void comm_process_init(void)
{
    comm_rs485_hk_init();
    comm_cmd_init();
}

void communication_process(void)
{
    /*
     * Commands first: a command that arrived this cycle is acted on before the
     * housekeeping frame that would report the resulting state goes out.
     */
    comm_cmd_service();
    comm_rs485_hk_service();
}
