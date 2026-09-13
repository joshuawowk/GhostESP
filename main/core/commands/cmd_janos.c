// cmd_janos.c
// Console commands for the external ESP32-C5 (JanOS) attached to the M5Stack
// Tab5 USB-A host port. Each verb sends a JanOS CLI line over USB CDC-ACM; the
// C5's line-based responses stream back to the terminal via the manager's
// default sink. c5scan additionally filters for 5 GHz results.

#include "core/commands.h"
#include "core/glog.h"
#include "managers/janos_usb_manager.h"
#include <stdio.h>
#include <string.h>

/* Join argv[first..argc) with single spaces into out (NUL-terminated). */
static void janos_join_args(int argc, char **argv, int first, char *out, size_t out_sz)
{
    out[0] = '\0';
    size_t len = 0;
    for (int i = first; i < argc && len < out_sz - 1; i++) {
        if (i > first && len < out_sz - 1) {
            out[len++] = ' ';
        }
        size_t n = strlen(argv[i]);
        if (n > out_sz - 1 - len) {
            n = out_sz - 1 - len;
        }
        memcpy(out + len, argv[i], n);
        len += n;
    }
    out[len] = '\0';
}

/* Send a fixed JanOS command line and report the result. */
static void janos_send_simple(const char *cmd)
{
    if (!janos_usb_manager_is_connected()) {
        glog("C5: not connected. Attach the JanOS C5 to the Tab5 USB-A port.\n");
        return;
    }
    if (janos_usb_manager_send_line(cmd)) {
        glog("C5 <- %s\n", cmd);
    } else {
        glog("C5: send failed\n");
    }
}

void handle_c5_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_usb_scan_networks_5ghz();
}

void handle_c5_results(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("show_scan_results");
}

void handle_c5_select(int argc, char **argv)
{
    if (argc < 2) {
        glog("Usage: c5select <index1> [index2] ...  (1-based, from c5scan)\n");
        return;
    }
    char cmd[128];
    char args[112];
    janos_join_args(argc, argv, 1, args, sizeof(args));
    snprintf(cmd, sizeof(cmd), "select_networks %s", args);
    janos_send_simple(cmd);
}

void handle_c5_deauth(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("start_deauth");
}

void handle_c5_sniff(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("start_sniffer");
}

void handle_c5_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("stop");
}

void handle_c5_hosts(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("list_hosts");
}

void handle_c5_pass(int argc, char **argv)
{
    char cmd[64];
    if (argc >= 2) {
        snprintf(cmd, sizeof(cmd), "show_pass %s", argv[1]);
    } else {
        snprintf(cmd, sizeof(cmd), "show_pass");
    }
    janos_send_simple(cmd);
}

void handle_c5_ping(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("ping");
}

void handle_c5_raw(int argc, char **argv)
{
    if (argc < 2) {
        glog("Usage: c5raw <JanOS command> [args...]\n");
        glog("Example: c5raw scan_networks\n");
        return;
    }
    char cmd[224];
    janos_join_args(argc, argv, 1, cmd, sizeof(cmd));
    janos_send_simple(cmd);
}
