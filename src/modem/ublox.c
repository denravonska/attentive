/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

#include <attentive/cellular.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "at-common.h"


#define UBLOX_WAITACK_TIMEOUT 60
#define UBLOX_FTP_TIMEOUT 60
#define UBLOX_LOCATE_TIMEOUT 150

static const char *const ublox_urc_responses[] = {
    "SRING: ",
    "#AGPSRING: ",
    NULL
};

struct cellular_ublox {
    struct cellular dev;

    int locate_status;
    float latitude, longitude, altitude;
};

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

    int status;
    if (sscanf(line, "#AGPSRING: %d", &status) == 1) {
        priv->locate_status = status;
        sscanf(line, "#AGPSRING: %*d,%f,%f,%f", &priv->latitude, &priv->longitude, &priv->altitude);
        return;
    }

    printf("[telit2@%p] urc: %.*s\n", priv, (int) len, line);
}

static const struct at_callbacks ublox_callbacks = {
    .scan_line = scan_line,
    .handle_urc = handle_urc,
};

static int ublox_attach(struct cellular *modem)
{
    at_set_callbacks(modem->at, &ublox_callbacks, (void *) modem);

    at_set_timeout(modem->at, 1);
    at_command(modem->at, "AT");        /* Aid autobauding. Always a good idea. */
    at_command(modem->at, "ATE0");      /* Disable local echo. */

    /* Initialize modem. */
    static const char *const init_strings[] = {
        "AT&K0",                        /* Disable hardware flow control. */
        "AT#SELINT=2",                  /* Set Telit module compatibility level. */
        "AT+CMEE=2",                    /* Enable extended error reporting. */
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
    at_set_timeout(modem->at, 5);
    at_command_simple(modem->at, "AT+CGDCONT=1,IP,\"%s\"", apn);

    at_set_timeout(modem->at, 150);
    const char *response = at_command(modem->at, "AT#SGACT=1,1");

    if (response == NULL)
        return -1;

    if (!strcmp(response, "+CME ERROR: context already activated"))
        return 0;

    int ip[4];
    at_simple_scanf(response, "#SGACT: %d.%d.%d.%d", &ip[0], &ip[1], &ip[2], &ip[3]);

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
    at_simple_scanf(response, "+USOCR: %d", &socket_id);
    return socket_id;
}

static int ublox_socket_connect(struct cellular *modem, int connid, const char *host, uint16_t port)
{
    /* Reset socket configuration to default. */
    at_set_timeout(modem->at, 5);
    at_command_simple(modem->at, "AT+USOCO=%d,\"%s\",%d", connid, host, port);
    return 0;
}

static ssize_t ublox_socket_send(struct cellular *modem, int connid, const void *buffer, size_t amount, int flags)
{
    (void) flags;
    const char *response;

    /* Request transmission. */
    at_set_timeout(modem->at, 150);
    at_expect_dataprompt(modem->at);
    response = at_command(modem->at, "AT+USOWR=%d,%zu", connid, amount);

    /* Wait for @ prompt */
    if(response == NULL || response[0] != '@')
        return 0;

    /* Send raw data. */
    response = at_command_raw(modem->at, buffer, amount);

    int bytes_written = 0;
    at_simple_scanf(response, "+USOWR: %*d,%d", &bytes_written);

    return bytes_written;
}

static ssize_t ublox_socket_recv(struct cellular *modem, int connid, void *buffer, size_t length, int flags)
{
    (void) flags;

    at_set_timeout(modem->at, 150);
    const char *response = at_command(modem->at, "AT+USORD=%d,%zu", connid, length);
    if(response == NULL)
        return 0;

    int cnt;
    at_simple_scanf(response, "+USORD: %*d,%d", &cnt);
    if(cnt <= 0)
        return 0;

    const char* data = strchr(response, '"');
    if(data == NULL)
        return 0;

    memcpy((char *)buffer, data, cnt);
    return cnt;
}

static int ublox_socket_close(struct cellular *modem, int connid)
{
    at_set_timeout(modem->at, 150);
    at_command_simple(modem->at, "AT+USOCL=%d", connid);

    return 0;
}

static int ublox_ftp_open(struct cellular *modem, const char *host, uint16_t port, const char *username, const char *password, bool passive)
{
    cellular_command_simple_pdp(modem, "AT#FTPOPEN=%s:%d,%s,%s,%d", host, port, username, password, passive);

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
    struct cellular_ublox *priv = (struct cellular_ublox *) modem;

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
    return -1;
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
    free(modem);
}

/* vim: set ts=4 sw=4 et: */
