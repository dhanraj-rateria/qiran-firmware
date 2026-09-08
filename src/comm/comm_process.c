#include "qiran/comm/comm_process.h"

#include "qiran/comm/comm_rs485_hk.h"

void comm_process_init(void)
{
    comm_rs485_hk_init();
}

void communication_process(void)
{
    comm_rs485_hk_service();
}
