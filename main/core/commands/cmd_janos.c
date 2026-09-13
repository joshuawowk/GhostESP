// cmd_janos.c
// Console commands for the external ESP32-C5 (JanOS) attached to the M5Stack
// Tab5 USB-A host port. GhostESP is the USB-CDC host; JanOS speaks a line-based
// text CLI (103 commands). Every verb sends a JanOS command and the C5's
// responses stream back to the terminal via the manager's default sink.
//
// `c5` / `c5raw` is a universal passthrough that exposes ALL of JanOS; the named
// verbs below are convenience shortcuts for the headline features (notably the
// 5 GHz Wi-Fi work the onboard 2.4 GHz-only C6 cannot do). `c5help` streams
// JanOS's own command list so the full surface is discoverable from GhostESP.

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

/* Send "<verb> <joined args>" (or just "<verb>" when no args). */
static void janos_send_with_args(const char *verb, int argc, char **argv)
{
    char cmd[224];
    if (argc >= 2) {
        char args[192];
        janos_join_args(argc, argv, 1, args, sizeof(args));
        snprintf(cmd, sizeof(cmd), "%s %s", verb, args);
    } else {
        snprintf(cmd, sizeof(cmd), "%s", verb);
    }
    janos_send_simple(cmd);
}

/* ---- status / discovery ---------------------------------------------- */

void handle_c5_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (janos_usb_manager_is_connected()) {
        glog("C5: connected on USB-A (JanOS radio peer). Use c5help for commands.\n");
    } else {
        glog("C5: not connected. Attach the JanOS C5 to the Tab5 USB-A port.\n");
    }
}

void handle_c5_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("help"); /* JanOS streams its full command list */
}

void handle_c5_version(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("version");
}

void handle_c5_ping(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("ping"); /* expect 'pong' streamed back */
}

/* ---- universal passthrough ------------------------------------------- */

void handle_c5_raw(int argc, char **argv)
{
    if (argc < 2) {
        glog("Usage: c5 <JanOS command> [args...]   (see c5help for the full list)\n");
        glog("Example: c5 start_karma   |   c5 wifi_connect MySSID MyPass\n");
        return;
    }
    char cmd[224];
    janos_join_args(argc, argv, 1, cmd, sizeof(cmd));
    janos_send_simple(cmd);
}

/* ---- 5 GHz Wi-Fi (the C5's headline capability) ---------------------- */

void handle_c5_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_usb_scan_networks_5ghz(); /* scan_networks + filter to 5 GHz rows */
}

void handle_c5_scanall(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("scan_networks"); /* full dual-band scan, streamed raw */
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
        glog("Usage: c5select <index1> [index2] ...  (1-based, from c5scan/c5scanall)\n");
        return;
    }
    janos_send_with_args("select_networks", argc, argv);
}

void handle_c5_deauth(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("start_deauth"); /* on selected networks; c5stop to end */
}

void handle_c5_handshake(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("start_handshake");
}

void handle_c5_sniff(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("start_sniffer");
}

void handle_c5_karma(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("start_karma");
}

void handle_c5_beacon(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("start_beacon_spam");
}

void handle_c5_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("stop"); /* universal cancel for any running C5 operation */
}

/* ---- recon / other radios / creds ------------------------------------ */

void handle_c5_hosts(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("list_hosts");
}

void handle_c5_probes(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("show_probes");
}

void handle_c5_bt(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("scan_bt");
}

void handle_c5_airtag(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("scan_airtag");
}

void handle_c5_wardrive(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("start_wardrive");
}

void handle_c5_pass(int argc, char **argv)
{
    janos_send_with_args("show_pass", argc, argv); /* [portal|evil] */
}

void handle_c5_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    janos_send_simple("reboot");
}
