#ifndef QIRAN_COMM_PROCESS_H
#define QIRAN_COMM_PROCESS_H

#include "qiran/qiran_types.h"

/*
 * Brings up the communication modules and connects the packet router's command
 * class to the command handler, so a command arriving inside a space packet
 * joins the same sequence as one arriving framed on the housekeeping link.
 *
 * There is no dispatch function here: the executive calls each communication
 * task directly, in its own place in the cycle.
 */
void comm_process_init(void);

#endif
