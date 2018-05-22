#pragma once

#include <attentive/cellular.h>

/**
 * Trigger socket status callback if assigned.
 *
 * @param modem Cellular modem instance.
 * @param connid Connection id.
 * @param status Socket status.
 */
void cellular_notify_socket_status(struct cellular *modem, int connid, enum socket_status status);
