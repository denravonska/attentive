/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

#include <attentive/cellular.h>
#include "cellular_priv.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "at-common.h"

#define UBLOX_NUM_SOCKETS 8
#define UBLOX_AUTOBAUD_ATTEMPTS 10
#define UBLOX_WAITACK_TIMEOUT 60
#define UBLOX_FTP_TIMEOUT 60
#define UBLOX_LOCATE_TIMEOUT 150
#define UBLOX_USOCO_TIMEOUT 20

static int ublox_socket_close(struct cellular *modem, int connid);

static const char *const ublox_urc_responses[] = {
    "+UUSOCL: ",        /* Socket disconnected */
    "+UUSORD: ",        /* Data received on socket */
    "+UUPSDA: ",        /* PDP context activation | deactivation aborted */
    "+UUPSDD: ",        /* PDP context closed */
    "+CRING: ",         /* Ring */
    NULL
};

struct ublox_socket {
   int16_t bytes_available;
   enum socket_status status;
};

struct cellular_ublox {
    struct cellular dev;
    struct ublox_socket socket[UBLOX_NUM_SOCKETS];
};

static int is_valid_socket(int id) {
   return id >= 0 && id < UBLOX_NUM_SOCKETS;
}

static char character_handler_usord(char ch, char *line, size_t len, void *arg) {
    struct at *priv = (struct at *) arg;

    int read;
    if(ch == ',') {
        line[len] = '\0';
        if (sscanf(line, "+USORD: %*d,%d,", &read) == 1) {
            at_set_character_handler(priv, NULL);
            ch = '\n';
        }
    }

    return ch;
}

static int scanner_usord(const char *line, size_t len, void *arg) {
    (void) len;
    (void) arg;

    int read;
    if (sscanf(line, "+USORD: %*d,%d", &read) == 1)
        if (read > 0) {
            return AT_RESPONSE_RAWDATA_FOLLOWS(read + 2);
        }

    return AT_RESPONSE_UNKNOWN;
}

static enum at_response_type scan_line(const char *line, size_t len, void *arg) {
    (void) line;
    (void) len;

    struct cellular_ublox *priv = arg;
    (void) priv;

    if (at_prefix_in_table(line, ublox_urc_responses))
        return AT_RESPONSE_URC;

    return AT_RESPONSE_UNKNOWN;
}

static void handle_urc(const char *line, size_t len, void *arg) {
    struct cellular_ublox *priv = arg;

    /*{
    int status;
    if (sscanf(line, "#AGPSRING: %d", &status) == 1) {
        priv->locate_status = status;
        sscanf(line, "#AGPSRING: %*d,%f,%f,%f", &priv->latitude, &priv->longitude, &priv->altitude);
        return;
    }
    }*/

    // Socket events
    int connid = -1;

    // Socket data available
    int length = 0;
    if(sscanf(line, "+UUSORD: %d,%d", &connid, &length) == 2 && is_valid_socket(connid)) {
       priv->socket[connid].bytes_available = length;
       return;
    }

    // Socket close
    if(sscanf(line, "UUSOCL: %d", &connid) == 1 && is_valid_socket(connid)) {
       priv->socket[connid].status = SOCKET_STATUS_UNKNOWN;
       cellular_notify_socket_status(&priv->dev, connid, priv->socket[connid].status);
       return;
    }
    
    // PDP context close
    int context = -1;
    if(sscanf(line, "UUPSDD: %d", &context) == 1) {
       if(priv->dev.cbs->pdp_deactivate_handler)
          priv->dev.cbs->pdp_deactivate_handler(context, priv->dev.arg);
       
       // Manual states that sockets are now invalid and must be closed.
       for(int i=0; i<UBLOX_NUM_SOCKETS; ++i)          
          if(priv->socket[i].status == SOCKET_STATUS_CONNECTED)
             ublox_socket_close(&priv->dev, i);
    }

    //printf("[ublox@%p] urc: %.*s\n", priv, (int) len, line);
}

static const struct at_callbacks ublox_callbacks = {
    .scan_line = scan_line,
    .handle_urc = handle_urc,
};

static int ublox_attach(struct cellular *modem)
{
    at_set_callbacks(modem->at, &ublox_callbacks, (void *) modem);
    at_set_timeout(modem->at, 1);

    /* Perform autobauding. */
    for (int i=0; i<UBLOX_AUTOBAUD_ATTEMPTS; i++)
    {
        const char *response = at_command(modem->at, "AT");
        if (response != NULL)
        {
            // Modem replied.
            break;
        }
    }

    // Disable local echo.
    at_command(modem->at, "ATE0");

    // Disable local echo again; make sure it was disabled successfully.
    at_command_simple(modem->at, "ATE0");

    /* Initialize modem. */
    static const char *const init_strings[] = {
//        "AT+IPR=0",                   /* Enable autobauding if not already enabled. */
        //"AT+IFC=0,0",                   /* Disable hardware flow control. */
        "AT+CMEE=2",                    /* Enable extended error reporting. */
        "AT&W0",                        /* Save configuration. */
        NULL
    };

    for (const char *const *command=init_strings; *command; command++)
        at_command_simple(modem->at, "%s", *command);

    return 0;
}

static int ublox_detach(struct cellular *modem)
{
    at_set_callbacks(modem->at, NULL, NULL);
    return 0;
}

static int ublox_pdp_open(struct cellular *modem, const char *apn)
{
    modem->apn = apn;
    /*at_set_timeout(modem->at, 5);
    at_command_simple(modem->at, "AT+CGDCONT=1,\"IP\",\"%s\"", apn);

    at_set_timeout(modem->at, 15);
    const char *response = at_command(modem->at, "AT+CGACT=1,1");

    if (response == NULL)
        return -1;

    if (strstr(response, "ERROR"))
    {
    if (!strcmp(response, "+CME ERROR: context already activated"))
        return 0;

       // Unrecoverable error.
       return -1;
    }*/

    // Check if already attached
    const char* response = at_command(modem->at, "AT+UPSND=0,8");
    if(response)
    {
        int status = 0;
        at_simple_scanf(response, "+UPSND: 0,8,%d", &status);
        if(status == 1)
            return 0;
    }

    // Setup packet switched data configuration for context 1.
    at_command_simple(modem->at, "AT+UPSD=0,1,\"%s\"", apn);
    at_command_simple(modem->at, "AT+UPSD=0,0,0");

    // Set up dynamic IP address assignment.
    //at_command_simple(modem->at, "AT+UPSD=0,7,\"0.0.0.0\"");

    // Set up the authentication protocol
    //at_command_simple(modem->at, "AT+UPSD=0,6,0");

    // Activate connection.
    at_set_timeout(modem->at, 15);
    at_command_simple(modem->at, "AT+UPSDA=0,3");

    return 0;
}

static int ublox_pdp_close(struct cellular *modem)
{
    at_set_timeout(modem->at, 150);
    at_command_simple(modem->at, "AT#SGACT=1,0");

    return 0;
}

static int ublox_op_iccid(struct cellular *modem, char *buf, size_t len)
{
    char fmt[24];
    if (snprintf(fmt, sizeof(fmt), "#CCID: %%[0-9]%ds", (int) len) >= (int) sizeof(fmt)) {
        errno = ENOSPC;
        return -1;
    }

    at_set_timeout(modem->at, 5);
    const char *response = at_command(modem->at, "AT#CCID");
    at_simple_scanf(response, fmt, buf);
    buf[len-1] = '\0';

    return 0;
}

//static int ublox_op_clock_gettime(struct cellular *modem, struct timespec *ts)
//{
//    struct tm tm;
//    int offset;

//    at_set_timeout(modem->at, 1);
//    const char *response = at_command(modem->at, "AT+CCLK?");
//    memset(&tm, 0, sizeof(struct tm));
//    at_simple_scanf(response, "+CCLK: \"%d/%d/%d,%d:%d:%d%d\"",
//            &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
//            &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
//            &offset);

//    /* Most modems report some starting date way in the past when they have
//     * no date/time estimation. */
//    if (tm.tm_year < 14) {
//        errno = EINVAL;
//        return 1;
//    }

//    /* Adjust values and perform conversion. */
//    tm.tm_year += 2000 - 1900;
//    tm.tm_mon -= 1;
//    time_t unix_time = timegm(&tm);
//    if (unix_time == -1) {
//        errno = EINVAL;
//        return -1;
//    }

//    /* Telit modems return local date/time instead of UTC (as defined in 3GPP
//     * 27.007). Remove the timezone shift. */
//    unix_time -= 15*60*offset;

//    /* All good. Return the result. */
//    ts->tv_sec = unix_time;
//    ts->tv_nsec = 0;
//    return 0;
//}

static int ublox_socket_create(struct cellular *modem, enum socket_type type)
{
    /* Reset socket configuration to default. */
    at_set_timeout(modem->at, 5);

    const char* response = at_command(modem->at, "AT+USOCR=%d", type == TCP_SOCKET ? 6 : 17);
    if(response == NULL)
        return -1;

    int socket_id = -1;
    if(sscanf(response, "+USOCR: %d", &socket_id))
    {
       // Enable keepalive
       at_command_simple(modem->at, "AT+USOSO=%d,65535,8,1", socket_id);

       // Enable nodelay
       //if(type == TCP_SOCKET)
//         at_command_simple(modem->at, "AT+USOSO=%d,6,1,1", socket_id);
    }

    return socket_id;
}

static int ublox_socket_connect(struct cellular *modem, int connid, const char *host, uint16_t port)
{
   struct cellular_ublox *priv = (struct cellular_ublox*) modem;

   if(!is_valid_socket(connid))
       return -1;

    at_set_timeout(modem->at, UBLOX_USOCO_TIMEOUT);
    at_command_simple(modem->at, "AT+USOCO=%d,\"%s\",%d", connid, host, port);
    priv->socket[connid].status = SOCKET_STATUS_CONNECTED;
    priv->socket[connid].bytes_available = 0;
    cellular_notify_socket_status(modem, connid, priv->socket[connid].status);

    return 0;
}

static ssize_t ublox_socket_send(struct cellular *modem, int connid, const void *buffer, size_t amount, int flags)
{
    (void) flags;

   if(!is_valid_socket(connid))
       return SOCKET_NOT_VALID;

    struct cellular_ublox *priv = (struct cellular_ublox*) modem;
    if(priv->socket[connid].status != SOCKET_STATUS_CONNECTED)
       return SOCKET_NOT_CONNECTED;
   
    if(amount <= 0)
       return 0;
    
    /* Request transmission. */
    at_set_timeout(modem->at, 5);
    at_expect_dataprompt(modem->at, "@");
    at_command_simple(modem->at, "AT+USOWR=%d,%d", connid, (uint32_t) amount);

    /* Send raw data. */
    const char* response = at_command_raw(modem->at, buffer, amount);
    int bytes_written = 0;
    if(response == NULL || sscanf(response, "+USOWR: %*d,%d", &bytes_written) != 1)
       return SOCKET_ERROR;

    return bytes_written;
}

static ssize_t ublox_socket_recv(struct cellular *modem, int connid, void *buffer, size_t length, int flags) {
   (void) flags;

   if(!is_valid_socket(connid))
       return SOCKET_NOT_VALID;

    struct cellular_ublox *priv = (struct cellular_ublox*) modem;
    if(priv->socket[connid].status != SOCKET_STATUS_CONNECTED)
       return SOCKET_NOT_CONNECTED;

    if(length <= 0)
       return 0;
    
    at_set_timeout(modem->at, 5);
    at_set_character_handler(modem->at, character_handler_usord);
    at_set_command_scanner(modem->at, scanner_usord);
    
    const char *response = at_command(modem->at, "AT+USORD=%d,%d", connid, (uint32_t) length);
    unsigned int bytes_read;
    if(response == NULL || sscanf(response, "+USORD: %*d,%d", &bytes_read) != 1)
       return SOCKET_ERROR;

    if(bytes_read <= 0)
        return bytes_read;

    // Locate payload in data.
    // +USORD: 0,95,"<data>"
    const char* data = strchr(response, '"');
    if(data == NULL)
       return -4;

    memcpy((char *)buffer, data + 1, bytes_read);
    priv->socket[connid].bytes_available -= bytes_read;

    return bytes_read;
}

static int ublox_socket_close(struct cellular *modem, int connid)
{
    if(!is_valid_socket(connid))
        return -1;

    struct cellular_ublox *priv = (struct cellular_ublox*) modem;
    priv->socket[connid].status = SOCKET_STATUS_UNKNOWN;
    at_set_timeout(modem->at, 15);
    at_command_simple(modem->at, "AT+USOCL=%d", connid);

    // Let UUSOCL URC trigger the callback.

    return 0;
}

static int ublox_socket_available(struct cellular *modem, int connid)
{
    if(!is_valid_socket(connid))
        return -1;    

    struct cellular_ublox *priv = (struct cellular_ublox*) modem;
    return priv->socket[connid].status == SOCKET_STATUS_CONNECTED
          ? priv->socket[connid].bytes_available
          : -1;
}

static int ublox_socket_status(struct cellular *modem, int connid)
{
   if(!is_valid_socket(connid))
       return SOCKET_STATUS_ERROR;

   struct cellular_ublox *priv = (struct cellular_ublox*) modem;
   return priv->socket[connid].status;
}

static int ublox_ftp_open(struct cellular *modem, const char *host, uint16_t port, const char *username, const char *password, bool passive)
{
    //cellular_command_simple_pdp(modem, "AT#FTPOPEN=%s:%d,%s,%s,%d", host, port, username, password, passive);
    return 0;
}

static int ublox_ftp_get(struct cellular *modem, const char *filename)
{
    at_set_timeout(modem->at, 90);
    at_command_simple(modem->at, "AT#FTPGETPKT=\"%s\",0", filename);

    return 0;
}

static enum at_response_type scanner_ftprecv(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    int bytes;
    if (sscanf(line, "#FTPRECV: %d", &bytes) == 1)
        return AT_RESPONSE_RAWDATA_FOLLOWS(bytes);
    return AT_RESPONSE_UNKNOWN;
}

static int ublox_ftp_getdata(struct cellular *modem, char *buffer, size_t length)
{
    /* FIXME: This function's flow is really ugly. */
    int retries = 0;
retry:
    at_set_timeout(modem->at, 150);
    at_set_command_scanner(modem->at, scanner_ftprecv);
    const char *response = at_command(modem->at, "AT#FTPRECV=%zu", length);

    if (response == NULL)
        return -1;

    int bytes;
    if (sscanf(response, "#FTPRECV: %d", &bytes) == 1) {
        /* Zero means no data is available. Wait for it. */
        if (bytes == 0) {
            /* Bail out on timeout. */
            if (++retries >= UBLOX_FTP_TIMEOUT) {
                errno = ETIMEDOUT;
                return -1;
            }
            sleep(1);
            goto retry;
        }

        /* Locate the payload. */
        const char *data = strchr(response, '\n');
        if (data == NULL) {
            errno = EPROTO;
            return -1;
        }
        data += 1;

        /* Copy payload to result buffer. */
        memcpy(buffer, data, bytes);
        return bytes;
    }

    /* Error or EOF? */
    int eof;
    response = at_command(modem->at, "AT#FTPGETPKT?");
    /* Expected response: #FTPGETPKT: <remotefile>,<viewMode>,<eof> */
#if 0
    /* The %[] specifier is not supported on some embedded systems. */
    at_simple_scanf(response, "#FTPGETPKT: %*[^,],%*d,%d", &eof);
#else
    /* Parse manually. */
    if (response == NULL)
        return -1;
    errno = EPROTO;
    /* Check the initial part of the response. */
    if (strncmp(response, "#FTPGETPKT: ", 12))
        return -1;
    /* Skip the filename. */
    response = strchr(response, ',');
    if (response == NULL)
        return -1;
    response++;
    at_simple_scanf(response, "%*d,%d", &eof);
#endif

    if (eof == 1)
        return 0;

    return -1;
}

static int ublox_locate(struct cellular *modem, float *latitude, float *longitude, float *altitude)
{
  /*  struct cellular_ublox *priv = (struct cellular_ublox *) modem;

    priv->locate_status = -1;
    at_set_timeout(modem->at, 150);
    cellular_command_simple_pdp(modem, "AT#AGPSSND");

    for (int i=0; i<UBLOX_LOCATE_TIMEOUT; i++) {
        sleep(1);
        if (priv->locate_status == 200) {
            *latitude = priv->latitude;
            *longitude = priv->longitude;
            *altitude = priv->altitude;
            return 0;
        }
        if (priv->locate_status != -1) {
            errno = ECONNABORTED;
            return -1;
        }
    }

    errno = ETIMEDOUT;
    return -1;*/
    return 0;
}

static int ublox_ftp_close(struct cellular *modem)
{
    at_set_timeout(modem->at, 90);
    at_command_simple(modem->at, "AT#FTPCLOSE");

    return 0;
}

static const struct cellular_ops ublox_ops = {
    .attach = ublox_attach,
    .detach = ublox_detach,

    .pdp_open = ublox_pdp_open,
    .pdp_close = ublox_pdp_close,

    .imei = cellular_op_imei,
    .iccid = ublox_op_iccid,
    .creg = cellular_op_creg,
    .rssi = cellular_op_rssi,
    //.clock_gettime = ublox_op_clock_gettime,
    //.clock_settime = cellular_op_clock_settime,
    .socket_create = ublox_socket_create,
    .socket_connect = ublox_socket_connect,
    .socket_send = ublox_socket_send,
    .socket_recv = ublox_socket_recv,
    .socket_close = ublox_socket_close,
    .socket_available = ublox_socket_available,
    .socket_status = ublox_socket_status,
    .ftp_open = ublox_ftp_open,
    .ftp_get = ublox_ftp_get,
    .ftp_getdata = ublox_ftp_getdata,
    .ftp_close = ublox_ftp_close,
    .locate = ublox_locate,
};

struct cellular *cellular_ublox_alloc(void)
{
    struct cellular_ublox *modem = malloc(sizeof(struct cellular_ublox));
    if (modem == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    memset(modem, 0, sizeof(*modem));
    modem->dev.ops = &ublox_ops;
    return (struct cellular *) modem;
}

void cellular_ublox_free(struct cellular *modem)
{
    free((struct cellular_ublox*) modem);
}

/* vim: set ts=4 sw=4 et: */
