#ifndef JANOS_USB_MANAGER_H
#define JANOS_USB_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * JanOS USB manager
 * -----------------
 * Drives an external ESP32-C5 running JanOS firmware, connected to the M5Stack
 * Tab5's USB-A host port. The ESP32-P4 acts as a USB CDC-ACM host and speaks
 * JanOS's plain-text CLI: commands are '\r\n'-terminated lines sent to the C5,
 * responses are line-based text streamed back.
 *
 * All symbols compile to no-ops unless CONFIG_JANOS_USB is set (Tab5 only).
 */

/* Line callback: invoked (from the CDC host task) for each complete text line
 * received from JanOS while no internal collector is active. `line` is NUL-
 * terminated and has the trailing CR/LF stripped; it is only valid for the
 * duration of the call. */
typedef void (*janos_line_cb_t)(const char *line, void *user_arg);

/* Install the USB host + CDC-ACM stack and start the connect/RX task. Safe to
 * call once at boot even with no C5 attached; the C5 is opened on hot-plug. */
void janos_usb_manager_init(void);

/* True once a JanOS C5 is enumerated, opened and line-coding-configured. */
bool janos_usb_manager_is_connected(void);

/* Send one CLI line to JanOS (a trailing "\r\n" is appended). Returns false if
 * no C5 is connected or the transmit fails. */
bool janos_usb_manager_send_line(const char *line);

/* Replace the default line sink (which streams to the terminal view). Pass NULL
 * to restore terminal streaming. */
void janos_usb_manager_set_line_callback(janos_line_cb_t cb, void *user_arg);

/* Run `scan_networks` on the C5, wait for the "Scan results printed" marker,
 * then print only the 5 GHz rows to the terminal. Returns false if not
 * connected or the scan times out. Blocks the caller up to ~30 s. */
bool janos_usb_scan_networks_5ghz(void);

/* Per-AP callback for janos_usb_scan_collect(). All pointers are valid only for
 * the duration of the call. */
typedef void (*janos_ap_cb_t)(const char *ssid, const uint8_t bssid[6],
                              uint8_t channel, int8_t rssi, const char *security,
                              void *ctx);

/* Run a C5 scan and invoke `cb` for each parsed AP (only the 5 GHz APs when
 * only_5ghz is true). Returns the number of APs reported, or -1 on error/not
 * connected. Blocks the caller for the duration of the scan (~15-20 s). Used to
 * merge the C5's 5 GHz results into GhostESP's own scan list. */
int janos_usb_scan_collect(janos_ap_cb_t cb, void *ctx, bool only_5ghz);

#ifdef __cplusplus
}
#endif

#endif /* JANOS_USB_MANAGER_H */
