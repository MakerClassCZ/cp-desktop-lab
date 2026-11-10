// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "airlift.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "airlift_net.h"
#include "bus_trace.h"
#include "desktop_pins.h"
#include "fault.h"
#include "host_net.h"
#include "net_fault.h"

#define START 0xe0
#define END 0xee
#define ERROR 0xef
#define REPLY 0x80
#define COMMAND_MAX 2048
#define REPLY_MAX 4200
#define PARAMS_MAX 8

// The replies as nina-fw's main/CommandHandler.cpp builds them.
static const char _version[] = "3.3.0";
static const uint8_t _bssid[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

typedef enum {
    IDLE,           // ready: low
    COMMAND,        // selected, a command coming in
    WORKING,        // waiting on the network
    REPLY_READY,    // ready (low) with a reply
    SENDING,        // selected, the reply going out
} state_t;

static struct {
    bool present;
    int cs, busy, reset, gpio0;
    state_t state;
    bool selected, in_reset;
    uint8_t command[COMMAND_MAX];
    size_t command_len;
    uint8_t reply[REPLY_MAX];
    size_t reply_len, reply_pos;
    uint8_t working;            // the command a job answers
    bool connected;
    uint8_t failed;             // the status a failed connect left: 1 no SSID, 4 failed
    char ssid[33];
    bool said[256];             // unknown commands said on stderr
} _a = { .cs = -1, .busy = -1, .reset = -1, .gpio0 = -1 };

bool airlift_add(int cs, int busy, int reset, int gpio0, const char **why) {
    if (_a.present) {
        *why = "one AirLift on a board";
        return false;
    }
    if (cs < 0 || busy < 0 || reset < 0) {
        *why = "an AirLift needs its cs=, busy= and reset= pins";
        return false;
    }
    _a.present = true;
    _a.cs = cs;
    _a.busy = busy;
    _a.reset = reset;
    _a.gpio0 = gpio0;
    return true;
}

bool airlift_uses_pin(int pin) {
    return _a.present && (pin == _a.cs || pin == _a.busy || pin == _a.reset || pin == _a.gpio0);
}

bool airlift_selected(void) {
    return _a.present && _a.selected;
}

bool airlift_pin_level(uint8_t pin, bool *level) {
    if (!_a.present || pin != _a.busy) {
        return false;
    }
    *level = _a.selected || _a.in_reset || _a.state == WORKING;
    return true;
}

static bool _driven_low(int pin, bool *output) {
    bool level;
    if (!desktop_virtual_pin((uint8_t)pin, output, &level)) {
        *output = false;
        return false;
    }
    return *output && !level;
}

// Replies
static void _begin(uint8_t command, uint8_t params) {
    _a.reply[0] = START;
    _a.reply[1] = command | REPLY;
    _a.reply[2] = params;
    _a.reply_len = 3;
}

static void _param(const void *data, size_t len) {
    _a.reply[_a.reply_len++] = (uint8_t)len;
    memcpy(_a.reply + _a.reply_len, data, len);
    _a.reply_len += len;
}

static void _end(void) {
    _a.reply[_a.reply_len++] = END;
    _a.state = REPLY_READY;
}

static void _byte(uint8_t command, uint8_t value) {
    _begin(command, 1);
    _param(&value, 1);
    _end();
}

static void _error(void) {
    _a.reply[0] = ERROR;
    _a.reply[1] = 0x00;
    _a.reply[2] = END;
    _a.reply_len = 3;
    _a.state = REPLY_READY;
}

// The network the computer is on, or the one the program asked for on Ethernet, as wifi does.
static void _network(char ssid[33], uint8_t bssid[6], int32_t *rssi, uint8_t *channel) {
    host_wifi_t wifi;
    if (host_wifi_current(&wifi)) {
        memcpy(ssid, wifi.ssid, 33);
        memcpy(bssid, wifi.bssid, 6);
        *rssi = wifi.rssi;
        *channel = (uint8_t)wifi.channel;
        return;
    }
    snprintf(ssid, 33, "%s", _a.connected ? _a.ssid : "CircuitPython host");
    memcpy(bssid, _bssid, 6);
    *rssi = -50;
    *channel = 6;
}

static void _connect(const uint8_t *ssid, size_t len) {
    len = len < sizeof(_a.ssid) - 1 ? len : sizeof(_a.ssid) - 1;
    memcpy(_a.ssid, ssid, len);
    _a.ssid[len] = '\0';
    // "fault wifi.connect": the status nina-fw reports, raised as the library raises it.
    fault_t f = fault_check("wifi.connect", _a.ssid, -1);
    _a.connected = !f.fired;
    _a.failed = !f.fired ? 0 : strcmp(f.action, "noap") == 0 ? 1 : 4;
    if (f.fired) {
        return;
    }
    net_link_restore();
    bus_trace_text("AIRLIFT connect %s", _a.ssid);
}

static uint16_t _le16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static void _handle(void) {
    uint8_t *c = _a.command;
    if (_a.command_len < 4 || c[0] != START) {
        _a.state = IDLE;            // a select with nothing for the ESP32
        return;
    }
    uint8_t command = c[1];
    // The parameters: 16-bit lengths where the library sends them so.
    bool wide = command == 0x44 || command == 0x45 || command == 0x46;
    const uint8_t *p[PARAMS_MAX];
    size_t n[PARAMS_MAX];
    int count = c[2];
    size_t at = 3;
    for (int i = 0; i < count; i++) {
        size_t len = c[at++];
        if (wide) {
            len = len << 8 | c[at++];
        }
        if (i >= PARAMS_MAX || at + len > _a.command_len) {
            _error();
            return;
        }
        p[i] = c + at;
        n[i] = len;
        at += len;
    }
    for (int i = count; i < PARAMS_MAX; i++) {
        static const uint8_t none[2];
        p[i] = none;
        n[i] = 0;
    }
    uint8_t sock = p[0][0];
    char ssid[33];
    uint8_t bssid[6];
    int32_t rssi;
    uint8_t channel;
    switch (command) {
        case 0x10:      // SET_NET
        case 0x11:      // SET_PASSPHRASE
        case 0x12:      // SET_KEY
            _connect(p[0], n[0]);
            _byte(command, 1);
            break;
        case 0x14:      // SET_IP_CONFIG, DNS, hostname, power mode, debug: the computer's own
        case 0x15:
        case 0x16:
        case 0x17:
        case 0x1a:
        case 0x40:      // SET_CLI_CERT, SET_PK
        case 0x41:
        case 0x4a:      // WPA2 Enterprise
        case 0x4b:
        case 0x4c:
        case 0x4d:
        case 0x4e:
        case 0x4f:
        case 0x36:      // START_SCAN_NETWORKS
        case 0x2a:      // DATA_SENT_TCP: the write did it
            _byte(command, 1);
            break;
        case 0x20:      // GET_CONN_STATUS
            // Connected, the link lost (a fault dropped it), a connect that failed, or none.
            _byte(command, _a.connected ? (net_link_up() ? 3 : 5) : _a.failed ? _a.failed : 6);
            break;
        case 0x21: {    // GET_IPADDR
            host_net_info_t info = { 0 };
            host_net_info(&info);
            _begin(command, 3);
            _param(&info.address, 4);
            _param(&info.netmask, 4);
            _param(&info.gateway, 4);
            _end();
            break;
        }
        case 0x22: {    // GET_MACADDR: reversed, as the library turns it back
            host_net_info_t info = { 0 };
            uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 }, reversed[6];
            if (host_net_info(&info) && info.have_mac) {
                memcpy(mac, info.mac, 6);
            }
            for (int i = 0; i < 6; i++) {
                reversed[i] = mac[5 - i];
            }
            _begin(command, 1);
            _param(reversed, 6);
            _end();
            break;
        }
        case 0x23:      // GET_CURR_SSID
            _network(ssid, bssid, &rssi, &channel);
            _begin(command, 1);
            _param(ssid, _a.connected ? strlen(ssid) : 0);
            _end();
            break;
        case 0x24:      // GET_CURR_BSSID
        case 0x3c:      // GET_IDX_BSSID
            _network(ssid, bssid, &rssi, &channel);
            _begin(command, 1);
            _param(bssid, 6);
            _end();
            break;
        case 0x25:      // GET_CURR_RSSI
        case 0x32:      // GET_IDX_RSSI
            _network(ssid, bssid, &rssi, &channel);
            _begin(command, 1);
            _param(&rssi, 4);
            _end();
            break;
        case 0x26:      // GET_CURR_ENCT, GET_IDX_ENCT: WPA2
        case 0x33:
            _byte(command, 4);
            break;
        case 0x3d:      // GET_IDX_CHAN
            _network(ssid, bssid, &rssi, &channel);
            _byte(command, channel);
            break;
        case 0x27:      // SCAN_NETWORKS: the one network the computer is on
            _network(ssid, bssid, &rssi, &channel);
            _begin(command, 1);
            _param(ssid, strlen(ssid));
            _end();
            break;
        case 0x37:      // GET_FW_VERSION, with its NUL
            _begin(command, 1);
            _param(_version, sizeof(_version));
            _end();
            break;
        case 0x3b: {    // GET_TIME: 64 bits
            int64_t now = (int64_t)time(NULL);
            _begin(command, 1);
            _param(&now, 8);
            _end();
            break;
        }
        case 0x30:      // DISCONNECT
            _a.connected = false;
            _byte(command, 1);
            break;
        case 0x34: {    // REQ_HOST_BY_NAME
            char host[256];
            memcpy(host, p[0], n[0]);
            host[n[0]] = '\0';
            airlift_net_resolve(host);
            _a.working = command;
            _a.state = WORKING;
            break;
        }
        case 0x35: {    // GET_HOST_BY_NAME
            uint32_t address = airlift_net_resolved();
            _begin(command, 1);
            _param(&address, 4);
            _end();
            break;
        }
        case 0x3e: {    // PING
            uint32_t ip = 0;
            memcpy(&ip, p[0], n[0] == 4 ? 4 : 0);
            airlift_net_ping(ip, p[1][0]);
            _a.working = command;
            _a.state = WORKING;
            break;
        }
        case 0x3f:      // GET_SOCKET
            _byte(command, airlift_net_get_socket());
            break;
        case 0x2d: {    // START_CLIENT_TCP: (ip, port, socket, mode) or (host, ip, port, socket, mode)
            int h = count == 5 ? 1 : 0;
            char host[256] = "";
            uint32_t ip = 0;
            if (h) {
                memcpy(host, p[0], n[0]);
                host[n[0]] = '\0';
            }
            memcpy(&ip, p[h], n[h] == 4 ? 4 : 0);
            uint16_t port = (uint16_t)(p[h + 1][0] << 8 | p[h + 1][1]);
            if ((count != 4 && count != 5) || !airlift_net_start(p[h + 2][0], host, ip, port, p[h + 3][0])) {
                _begin(command, 0);
                _end();
                break;
            }
            _a.working = command;
            _a.state = WORKING;
            break;
        }
        case 0x2e:      // STOP_CLIENT_TCP
            airlift_net_stop(sock);
            _byte(command, 1);
            break;
        case 0x2f:      // GET_CLIENT_STATE_TCP
            _byte(command, airlift_net_state(sock));
            break;
        case 0x2b: {    // AVAIL_DATA_TCP
            uint16_t available = airlift_net_available(sock);
            _begin(command, 1);
            _param(&available, 2);
            _end();
            break;
        }
        case 0x2c: {    // GET_DATA_TCP: one byte, perhaps a peek
            uint8_t value = 0xff;
            airlift_net_read(sock, &value, 1, p[1][0] != 0);
            _byte(command, value);
            break;
        }
        case 0x44: {    // SEND_DATA_TCP
            uint16_t written = airlift_net_write(sock, p[1], (uint16_t)n[1]);
            _begin(command, 1);
            _param(&written, 2);
            _end();
            break;
        }
        case 0x45: {    // GET_DATABUF_TCP: the reply's length is 16 bits
            uint16_t want = n[1] >= 2 ? _le16(p[1]) : 0;
            if (want > REPLY_MAX - 8) {
                want = REPLY_MAX - 8;
            }
            int got = airlift_net_read(sock, _a.reply + 5, want, false);
            got = got < 0 ? 0 : got;
            _a.reply[0] = START;
            _a.reply[1] = command | REPLY;
            _a.reply[2] = 1;
            _a.reply[3] = (uint8_t)(got >> 8);
            _a.reply[4] = (uint8_t)got;
            _a.reply_len = 5 + (size_t)got;
            _end();
            break;
        }
        case 0x46:      // INSERT_DATABUF
            _byte(command, airlift_net_udp_append(sock, p[1], (uint16_t)n[1]));
            break;
        case 0x39:      // SEND_UDP_DATA
            _byte(command, airlift_net_udp_send(sock));
            break;
        case 0x3a: {    // GET_REMOTE_DATA: the port most significant byte first
            uint32_t ip;
            uint16_t port;
            airlift_net_remote(sock, &ip, &port);
            uint8_t port_bytes[2] = { (uint8_t)(port >> 8), (uint8_t)port };
            _begin(command, 2);
            _param(&ip, 4);
            _param(port_bytes, 2);
            _end();
            break;
        }
        case 0x28: {    // START_SERVER_TCP: (port, socket, mode) or (ip, port, socket, mode)
            int at_port = count == 4 ? 1 : 0;
            uint16_t port = (uint16_t)(p[at_port][0] << 8 | p[at_port][1]);
            _byte(command, (count == 3 || count == 4) && airlift_net_listen(p[at_port + 1][0], port, p[at_port + 2][0]));
            break;
        }
        case 0x29:      // GET_STATE_TCP
            _byte(command, airlift_net_server_state(sock));
            break;
        case 0x50:      // SET_PIN_MODE: the ESP32's own pins, its RGB LED on 25-27
            bus_trace_text("AIRLIFT pin %u mode %u", p[0][0], p[1][0]);
            _byte(command, 1);
            break;
        case 0x51:
            bus_trace_text("AIRLIFT pin %u = %u", p[0][0], p[1][0]);
            _byte(command, 1);
            break;
        case 0x52:
            bus_trace_text("AIRLIFT pin %u pwm %u", p[0][0], p[1][0]);
            _byte(command, 1);
            break;
        case 0x53:      // reads answer 0
            _byte(command, 0);
            break;
        case 0x54: {
            int32_t zero = 0;
            _begin(command, 1);
            _param(&zero, 4);
            _end();
            break;
        }
        case 0x18:      // an access point: not here
        case 0x19:
            _error();
            break;
        default:
            if (!_a.said[command]) {
                _a.said[command] = true;
                fprintf(stderr, "circuitpython: AirLift: nina-fw command 0x%02x is not modelled\n", command);
            }
            _error();
            break;
    }
}

// A job's result, as the command it answers replies it.
static void _finish(int result) {
    uint8_t command = _a.working;
    if (command == 0x2d) {
        if (result == 1) {
            _byte(command, 1);
        } else {
            _begin(command, 0);
            _end();
        }
    } else if (command == 0x3e) {
        int16_t ms = (int16_t)result;
        _begin(command, 1);
        _param(&ms, 2);
        _end();
    } else {
        _byte(command, result == 1);
    }
}

void airlift_vm_task(void) {
    if (!_a.present || _a.state != WORKING) {
        return;
    }
    int result;
    if (airlift_net_poll(&result)) {
        _finish(result);
    }
}

static void _restart(void) {
    _a.state = IDLE;
    _a.command_len = 0;
    _a.reply_len = 0;
    _a.connected = false;
    _a.failed = 0;
    airlift_net_reset();
}

void airlift_pin_changed(uint8_t pin) {
    if (!_a.present) {
        return;
    }
    bool output;
    if (pin == _a.reset) {
        bool low = _driven_low(pin, &output);
        if (low && !_a.in_reset) {
            // Only the program drives it low: a reset of the pins makes it an input.
            bus_trace_text("AIRLIFT reset");
            _restart();
        }
        _a.in_reset = low;
        return;
    }
    if (pin != _a.cs) {
        return;
    }
    bool selected = _driven_low(pin, &output);
    if (selected == _a.selected) {
        return;
    }
    _a.selected = selected;
    if (selected) {
        if (_a.in_reset) {
            return;
        }
        if (_a.state == IDLE) {
            _a.command_len = 0;
            _a.state = COMMAND;
        } else if (_a.state == REPLY_READY) {
            _a.reply_pos = 0;
            _a.state = SENDING;
        }
        return;
    }
    if (_a.state == COMMAND) {
        // A select given up by a reset of the pins carries no command.
        if (output) {
            _handle();
        } else {
            _a.state = IDLE;
        }
    } else if (_a.state == SENDING) {
        _a.state = IDLE;
    }
}

void airlift_transfer(const uint8_t *out, uint8_t fill, uint8_t *in, size_t len) {
    for (size_t k = 0; k < len; k++) {
        uint8_t back = 0;
        if (_a.state == COMMAND) {
            if (_a.command_len < COMMAND_MAX) {
                _a.command[_a.command_len++] = out != NULL ? out[k] : fill;
            }
        } else if (_a.state == SENDING && _a.reply_pos < _a.reply_len) {
            back = _a.reply[_a.reply_pos++];
        }
        if (in != NULL) {
            in[k] = back;
        }
    }
}

void airlift_reset(void) {
    if (_a.present) {
        _restart();
        _a.selected = false;
        _a.in_reset = false;
    }
}
