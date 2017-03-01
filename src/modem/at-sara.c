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

#include "FreeRTOS.h"
#include "task.h"

#include "at-common.h"
#define printf(...)
#include "debug.h"

/* Defines -------------------------------------------------------------------*/
DBG_SET_LEVEL(DBG_LEVEL_D);

#define AUTOBAUD_ATTEMPTS         10
#define WAITACK_TIMEOUT           40
#define TCP_CONNECT_TIMEOUT       40
#define SARA_NSOCKETS             6

enum socket_status {
    SOCKET_STATUS_ERROR = -1,
    SOCKET_STATUS_UNKNOWN = 0,
    SOCKET_STATUS_CONNECTED = 1,
};

static const char *const sara_urc_responses[] = {
    "+UUSOCL: ",        /* Socket disconnected */
    "+UUSORD: ",        /* Data received on socket */
    "+UUPSDA: ",        /* PDP context activation | deactivation aborted */
    "+UUPSDD: ",        /* PDP context closed */
    "+CRING: ",         /* Ring */
    /* "+PDP: DEACT",      [> PDP disconnected <] */
    /* "+SAPBR 1: DEACT",  [> PDP disconnected (for SAPBR apps) <] */
    /* "*PSNWID: ",        [> AT+CLTS network name <] */
    /* "*PSUTTZ: ",        [> AT+CLTS time <] */
    /* "+CTZV: ",          [> AT+CLTS timezone <] */
    /* "DST: ",            [> AT+CLTS dst information <] */
    /* "+CIEV: ",          [> AT+CLTS undocumented indicator <] */
    /* "RDY",              [> Assorted crap on newer firmware releases. <] */
    /* "+CPIN: READY", */
    /* "Call Ready", */
    /* "SMS Ready", */
    /* "NORMAL POWER DOWN", */
    /* "UNDER-VOLTAGE POWER DOWN", */
    /* "UNDER-VOLTAGE WARNNING", */
    /* "OVER-VOLTAGE POWER DOWN", */
    /* "OVER-VOLTAGE WARNNING", */
    NULL
};

struct cellular_sara {
    struct cellular dev;
    enum socket_status socket_status[SARA_NSOCKETS];
};

static enum at_response_type scan_line(const char *line, size_t len, void *arg)
{
    (void) len;
//    struct cellular_sara *priv = arg;

    if (at_prefix_in_table(line, sara_urc_responses)) {
        return AT_RESPONSE_URC;
    }

    return AT_RESPONSE_UNKNOWN;
}

static void handle_urc(const char *line, size_t len, void *arg)
{
    struct cellular_sara *priv = arg;

    DBG_D("U> %s\r\n", line);

    int connid = -1;
    if(sscanf(line, "UUSOCL: %d", &connid) == 1) {
        if(connid < SARA_NSOCKETS) {
            priv->socket_status[connid] = SOCKET_STATUS_UNKNOWN;
        }
    }
}

static const struct at_callbacks sara_callbacks = {
    .scan_line = scan_line,
    .handle_urc = handle_urc,
};


/**
 * sara IP configuration commands fail if the IP application is running,
 * even though the configuration settings are already right. The following
 * monkey dance is therefore needed.
 */
static int sara_config(struct cellular *modem, const char *option, const char *value, int attempts)
{
    for (int i=0; i<attempts; i++) {
        /* Blindly try to set the configuration option. */
        at_command(modem->at, "AT+%s=%s", option, value);

        /* Query the setting status. */
        const char *response = at_command(modem->at, "AT+%s?", option);
        /* Bail out on timeouts. */
        if (response == NULL)
            return -1;

        /* Check if the setting has the correct value. */
        char expected[16];
        if (snprintf(expected, sizeof(expected), "+%s: %s", option, value) >= (int) sizeof(expected)) {
            return -1;
        }
        if (!strcmp(response, expected))
            return 0;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return -1;
}


static int sara_attach(struct cellular *modem)
{
    at_set_callbacks(modem->at, &sara_callbacks, (void *) modem);

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);

    /* Perform autobauding. */
    for (int i=0; i<AUTOBAUD_ATTEMPTS; i++) {
        const char *response = at_command(modem->at, "AT");
        if (response != NULL) {
            break;
        }
    }

    /* Disable local echo. */
    at_command(modem->at, "ATE0");

    /* Disable local echo again; make sure it was disabled successfully. */
    at_command_simple(modem->at, "ATE0");

    /* Initialize modem. */
    static const char *const init_strings[] = {
        "AT+CMEE=2",                    /* Enable extended error reporting. */
        //"AT+CSGT=1,\"Ready\"",        /* Enable greeting message. */
        "AT+IPR=115200",                /* Set fixed baudrate. */
        NULL
    };
    for (const char *const *command=init_strings; *command; command++)
        at_command_simple(modem->at, "%s", *command);

    return 0;
}

static int sara_detach(struct cellular *modem)
{
    at_set_callbacks(modem->at, NULL, NULL);
    return 0;
}

static int sara_pdp_open(struct cellular *modem, const char *apn)
{
    int active = 0;
    /* Skip the configuration if context is already open. */
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+UPSND=0,8");
    at_simple_scanf(response, "+UPSND: 0,8,%d", &active);
    if(active) {
        return 0;
    }
    /* Configure and open internal pdp context. */
    at_command_simple(modem->at, "AT+UPSD=0,1,\"%s\"", apn); // TODO: add usr & pwd [Dale]
    /* at_command_simple(modem->at, "AT+UPSD=0,2,\"%s\"", usr); */
    /* at_command_simple(modem->at, "AT+UPSD=0,3,\"%s\"", pwd); */
    /* at_command_simple(modem->at, "AT+UPSD=0,6,\"%s\"", auth); */
    at_command_simple(modem->at, "AT+UPSD=0,7,\"0.0.0.0\"");
    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    at_command_simple(modem->at, "AT+UPSDA=0,3");
    /* Read local IP address. */
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *resp = at_command(modem->at, "AT+UPSND=0,0");
    if(resp == NULL) {
      return -1;
    }

    return 0;
}

static int sara_pdp_close(struct cellular *modem)
{
    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    at_command_simple(modem->at, "AT+UPSDA=0,4");

    return 0;
}


static int sara_socket_connect(struct cellular *modem, const char *host, uint16_t port)
{
    struct cellular_sara *priv = (struct cellular_sara *) modem;
    /* Open pdp context */
    if(cellular_pdp_request(modem) != 0) {
      return -1;
    }
    /* Create a tcp socket. */
    int connid = -1;
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+USOCR=6");
    at_simple_scanf(response, "+USOCR: %d", &connid);
    if(connid >= SARA_NSOCKETS) {
        return -1;
    }
    priv->socket_status[connid] = SOCKET_STATUS_UNKNOWN;
    /* Send connection request. */
    at_set_timeout(modem->at, TCP_CONNECT_TIMEOUT);
    at_command_simple(modem->at, "AT+USOCO=%d,\"%s\",%d", connid, host, port);
    priv->socket_status[connid] = SOCKET_STATUS_CONNECTED;

    return connid;
}

static ssize_t sara_socket_send(struct cellular *modem, int connid, const void *buffer, size_t amount, int flags)
{
    (void) flags;
    struct cellular_sara *priv = (struct cellular_sara *) modem;

    amount = amount > 1024 ? 1024 : amount;
    if(priv->socket_status[connid] != SOCKET_STATUS_CONNECTED) {
      return -1;
    }

    /* Request transmission. */
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_expect_dataprompt(modem->at, "@");
    at_command_simple(modem->at, "AT+USOWR=%d,%zu", connid, amount);
    vTaskDelay(pdMS_TO_TICKS(50));
    const char* resp = at_command_raw(modem->at, buffer, amount);
    int written = 0;
    if(resp == NULL || sscanf(resp, "+USOWR: %*d,%d", &written) != 1 || written != amount) {
      return -1;
    }

    return written;
}

static enum at_response_type scanner_usord(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    int read;
    if (sscanf(line, "+USORD: %*d,%d", &read) == 1)
        if (read > 0) {
            return AT_RESPONSE_RAWDATA_FOLLOWS(read + 2);
        }

    return AT_RESPONSE_UNKNOWN;
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

static ssize_t sara_socket_recv(struct cellular *modem, int connid, void *buffer, size_t length, int flags)
{
    (void) flags;

    struct cellular_sara *priv = (struct cellular_sara *) modem;
    int cnt = 0;

    // TODO its dumb and exceptions should be handled in other right way
    if(priv->socket_status[connid] != SOCKET_STATUS_CONNECTED) {
        DBG_I(">>>>DISCONNECTED\r\n");
        return -1;
    }
    char tries = 4;
    while ((cnt < (int)length) && tries--){
        int chunk = (int) length - cnt;
        /* Limit read size to avoid overflowing AT response buffer. */
        chunk = chunk > 480 ? 480 : chunk;

        /* Perform the read. */
        at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
        at_set_character_handler(modem->at, character_handler_usord);
        at_set_command_scanner(modem->at, scanner_usord);
        const char *response = at_command(modem->at, "AT+USORD=%d,%d", connid, chunk);
        if (response == NULL) {
            DBG_W(">>>>NO RESPONSE\r\n");
            return -1;
        }
        /* Find the header line. */
        int read;
        // TODO:
        // 1. connid is not checked
        // 2. there is possible a bug here.
        if(sscanf(response, "+USORD: %*d,%d", &read) != 1) {
            DBG_I(">>>>BAD RESPONSE\r\n");
            return -1;
        }

        /* Bail out if we're out of data. */
        /* FIXME: We should maybe block until we receive something? */
        if (read == 0) {
            break;
        }

        /* Locate the payload. */
        const char *data = strchr(response, '\n');
        if (data++ == NULL) {
            DBG_I(">>>>NO DATA\r\n");
            return -1;
        }

        /* Copy payload to result buffer. */
        memcpy((char *)buffer + cnt, ++data, read);
        cnt += read;
    }

    return cnt;
}

static int sara_socket_waitack(struct cellular *modem, int connid)
{
    struct cellular_sara *priv = (struct cellular_sara *) modem;

    if(priv->socket_status[connid] == SOCKET_STATUS_CONNECTED) {
        at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
        for (int i=0; i<WAITACK_TIMEOUT; i++) {
            /* Read number of bytes waiting. */
            int nack;
            const char *response = at_command(modem->at, "AT+USOCTL=%d,11", connid);
            at_simple_scanf(response, "+USOCTL: %*d,11,%d", &nack);

            /* Return if all bytes were acknowledged. */
            if (nack == 0) {
                return 0;
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    return -1;
}

static int sara_socket_close(struct cellular *modem, int connid)
{
    struct cellular_sara *priv = (struct cellular_sara *) modem;

    if(priv->socket_status[connid] == SOCKET_STATUS_CONNECTED) {
        at_set_timeout(modem->at, AT_TIMEOUT_LONG);
        at_command_simple(modem->at, "AT+USOCL=%d", connid);
    }

    return 0;
}

static int sara_op_cops(struct cellular *modem)
{
    int ops = -1;
    int rat = -1;

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command_simple(modem->at, "AT+COPS=3,2");
    const char *response = at_command(modem->at, "AT+COPS?");
    int ret = sscanf(response, "+COPS: %*d,%*d,\"%d\",%d", &ops, &rat);
    if(ret == 2) {
        ops |= rat << 24;
    }

    return ops;
}

static const struct cellular_ops sara_ops = {
    .attach = sara_attach,
    .detach = sara_detach,

    .pdp_open = sara_pdp_open,
    .pdp_close = sara_pdp_close,

    .imei = cellular_op_imei,
    .iccid = cellular_op_iccid,
    .imsi = cellular_op_imsi,
    .creg = cellular_op_creg,
    .rssi = cellular_op_rssi,
    .cops = sara_op_cops,
    .test = cellular_op_test,
    .ats0 = cellular_op_ats0,
    .sms = cellular_op_sms,
    .socket_connect = sara_socket_connect,
    .socket_send = sara_socket_send,
    .socket_recv = sara_socket_recv,
    .socket_waitack = sara_socket_waitack,
    .socket_close = sara_socket_close,
};

static struct cellular_sara cellular;

struct cellular *cellular_sara_alloc(void)
{
    struct cellular_sara *modem = &cellular;

    memset(modem, 0, sizeof(*modem));
    modem->dev.ops = &sara_ops;

    return (struct cellular *) modem;
}

void cellular_sara_free(struct cellular *modem)
{
}

/* vim: set ts=4 sw=4 et: */
