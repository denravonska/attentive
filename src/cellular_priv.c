#include "cellular_priv.h"

void cellular_notify_socket_status(struct cellular *modem, int connid, enum socket_status status)
{
    if(modem->cbs && modem->cbs->socket_status_handler)
       modem->cbs->socket_status_handler(
                connid,
                status,
                modem->arg);
}
