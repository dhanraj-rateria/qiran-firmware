#ifndef QIRAN_COMM_CMD_H
#define QIRAN_COMM_CMD_H

#include "qiran/qiran_types.h"

/*
 * Command sequence: format check, decode, legality check, parameter check, dispatch, result.
 * Owner: Major Task 2. Not yet implemented.
 */
qiran_status_t comm_cmd_init(void);

#endif
