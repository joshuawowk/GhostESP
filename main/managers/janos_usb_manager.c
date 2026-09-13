#include "sdkconfig.h"
#include "managers/janos_usb_manager.h"

#if defined(CONFIG_JANOS_USB)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "core/glog.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <string.h>
#include <stdio.h>

/*
 * JanOS ESP32-C5 over the Tab5 USB-A host port.
 *
 * The ESP32-P4 is the USB host. A C5 running JanOS enumerates as a standard
 * CDC-ACM device on its native USB-Serial-JTAG endpoint (VID 0x303A). We open
 * it, set 115200 8N1, and exchange JanOS's plain-text CLI: each command is a
 * "\r\n"-terminated line; responses stream back line-by-line.
 *
 * This is the ONLY usb_host_install() on the Tab5 build: the USB-keyboard
 * manager is ESP32-S3 only, and esp_video's USB host is gated behind
 * CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE (off here). If a future P4 build
 * enables another USB host, they must share a single usb_host_install.
 */

#define JANOS_C5_VID 0x303A /* Espressif native USB */
#define JANOS_C5_PID 0x1001 /* USB-Serial-JTAG CDC */
#define JANOS_BAUD 115200
#define JANOS_RX_LINE_MAX 512
#define JANOS_COLLECT_MAX 12288
#define JANOS_TX_MAX 256
#define JANOS_SCAN_TIMEOUT_MS 20000

static const char *TAG = "janos_usb";

static TaskHandle_t s_host_task = NULL;
static TaskHandle_t s_connect_task = NULL;
static volatile bool s_host_ready = false;
static volatile bool s_cdc_installed = false;

static cdc_acm_dev_hdl_t s_dev = NULL;
static volatile bool s_connected = false;

static SemaphoreHandle_t s_dev_mutex = NULL;     /* guards s_dev for tx + open/close */
static SemaphoreHandle_t s_collect_mutex = NULL; /* guards the collection buffer/flags */
static SemaphoreHandle_t s_collect_sem = NULL;   /* given when the collect marker is seen */
static SemaphoreHandle_t s_disc_sem = NULL;      /* given on device disconnect */

static janos_line_cb_t s_line_cb = NULL;
static void *s_line_arg = NULL;

/* RX line assembly (CDC callback context). */
EXT_RAM_BSS_ATTR static char s_rxline[JANOS_RX_LINE_MAX];
static size_t s_rxlen = 0;

/* Marker-driven line collector (used by scan). */
EXT_RAM_BSS_ATTR static char s_collect_buf[JANOS_COLLECT_MAX];
static size_t s_collect_len = 0;
static volatile bool s_collecting = false;
static char s_collect_marker[48];

/* ---- line dispatch --------------------------------------------------- */

static void janos_dispatch_line(const char *line)
{
    /* Collector path first: if a command armed a collector, capture the line
     * and signal completion when the marker appears. */
    if (s_collect_mutex) {
        xSemaphoreTake(s_collect_mutex, portMAX_DELAY);
        if (s_collecting) {
            size_t n = strlen(line);
            if (s_collect_len + n + 1 < JANOS_COLLECT_MAX) {
                memcpy(s_collect_buf + s_collect_len, line, n);
                s_collect_len += n;
                s_collect_buf[s_collect_len++] = '\n';
                s_collect_buf[s_collect_len] = '\0';
            }
            bool done = (s_collect_marker[0] && strstr(line, s_collect_marker) != NULL);
            xSemaphoreGive(s_collect_mutex);
            if (done && s_collect_sem) {
                xSemaphoreGive(s_collect_sem);
            }
            return;
        }
        xSemaphoreGive(s_collect_mutex);
    }

    /* Idle path: custom sink, else stream to the terminal. */
    if (s_line_cb) {
        s_line_cb(line, s_line_arg);
    } else {
        glog("%s\n", line);
    }
}

static bool janos_rx_cb(const uint8_t *data, size_t data_len, void *arg)
{
    (void)arg;
    for (size_t i = 0; i < data_len; i++) {
        char c = (char)data[i];
        if (c == '\n') {
            /* strip a trailing CR */
            if (s_rxlen && s_rxline[s_rxlen - 1] == '\r') {
                s_rxlen--;
            }
            s_rxline[s_rxlen] = '\0';
            if (s_rxlen) {
                janos_dispatch_line(s_rxline);
            }
            s_rxlen = 0;
        } else if (c == '\r') {
            /* keep; stripped on the next '\n' or dropped on overflow */
            if (s_rxlen < JANOS_RX_LINE_MAX - 1) {
                s_rxline[s_rxlen++] = c;
            }
        } else {
            if (s_rxlen < JANOS_RX_LINE_MAX - 1) {
                s_rxline[s_rxlen++] = c;
            } else {
                /* overrun: flush what we have as a line and continue */
                s_rxline[s_rxlen] = '\0';
                janos_dispatch_line(s_rxline);
                s_rxlen = 0;
                s_rxline[s_rxlen++] = c;
            }
        }
    }
    return true; /* data consumed */
}

static void janos_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGW(TAG, "CDC error %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "C5 disconnected");
        xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
        s_connected = false;
        if (s_dev == event->data.cdc_hdl) {
            s_dev = NULL;
        }
        cdc_acm_host_close(event->data.cdc_hdl);
        xSemaphoreGive(s_dev_mutex);
        glog("C5 (JanOS) disconnected from USB-A\n");
        if (s_disc_sem) {
            xSemaphoreGive(s_disc_sem);
        }
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
    case CDC_ACM_HOST_NETWORK_CONNECTION:
    default:
        break;
    }
}

/* ---- USB host + connect tasks --------------------------------------- */

static void janos_usb_host_task(void *arg)
{
    (void)arg;
    const usb_host_config_t cfg = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    if (usb_host_install(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed");
        s_host_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_host_ready = true;
    ESP_LOGI(TAG, "USB host ready (JanOS)");
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void janos_connect_task(void *arg)
{
    (void)arg;
    /* Wait for the host stack + CDC driver to be up. */
    while (!s_host_ready || !s_cdc_installed) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    const cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 3000,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .event_cb = janos_event_cb,
        .data_cb = janos_rx_cb,
        .user_arg = NULL,
    };

    for (;;) {
        if (!s_connected) {
            cdc_acm_dev_hdl_t dev = NULL;
            esp_err_t err = cdc_acm_host_open(JANOS_C5_VID, JANOS_C5_PID, 0, &dev_cfg, &dev);
            if (err == ESP_OK && dev) {
                xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
                s_dev = dev;
                s_connected = true;
                xSemaphoreGive(s_dev_mutex);

                cdc_acm_line_coding_t coding = {
                    .dwDTERate = JANOS_BAUD,
                    .bCharFormat = 0, /* 1 stop bit */
                    .bParityType = 0, /* none */
                    .bDataBits = 8,
                };
                cdc_acm_host_line_coding_set(dev, &coding);
                /* DTR/RTS: native USB-Serial-JTAG has no auto-reset circuit, so
                 * asserting these only tells the CLI a terminal is present. */
                cdc_acm_host_set_control_line_state(dev, true, true);

                ESP_LOGI(TAG, "C5 (JanOS) connected");
                glog("C5 (JanOS) connected on USB-A (115200 8N1)\n");

                /* Block until this device disconnects, then loop to reopen. */
                xSemaphoreTake(s_disc_sem, portMAX_DELAY);
            } else {
                /* No device yet (ESP_ERR_NOT_FOUND) or transient failure. */
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

/* ---- public API ------------------------------------------------------ */

void janos_usb_manager_init(void)
{
    if (s_host_task) {
        return; /* already initialised */
    }
    if (!s_dev_mutex) {
        s_dev_mutex = xSemaphoreCreateMutex();
    }
    if (!s_collect_mutex) {
        s_collect_mutex = xSemaphoreCreateMutex();
    }
    if (!s_collect_sem) {
        s_collect_sem = xSemaphoreCreateBinary();
    }
    if (!s_disc_sem) {
        s_disc_sem = xSemaphoreCreateBinary();
    }
    if (!s_dev_mutex || !s_collect_mutex || !s_collect_sem || !s_disc_sem) {
        ESP_LOGE(TAG, "sync primitive alloc failed");
        return;
    }

    s_host_ready = false;
    if (xTaskCreatePinnedToCore(janos_usb_host_task, "janos_usbh", 4096, NULL, 5, &s_host_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "host task create failed");
        s_host_task = NULL;
        return;
    }

    int wait = 0;
    while (!s_host_ready && s_host_task && wait < 50) {
        vTaskDelay(pdMS_TO_TICKS(20));
        wait++;
    }
    if (!s_host_ready) {
        ESP_LOGE(TAG, "USB host init timeout");
        return;
    }

    if (cdc_acm_host_install(NULL) != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed");
        return;
    }
    s_cdc_installed = true;

    if (xTaskCreatePinnedToCore(janos_connect_task, "janos_conn", 4096, NULL, 4, &s_connect_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "connect task create failed");
        s_connect_task = NULL;
        return;
    }
    ESP_LOGI(TAG, "JanOS USB manager started (waiting for C5 on USB-A)");
}

bool janos_usb_manager_is_connected(void)
{
    return s_connected;
}

bool janos_usb_manager_send_line(const char *line)
{
    if (!line || !s_dev_mutex) {
        return false;
    }
    size_t len = strlen(line);
    if (len > JANOS_TX_MAX - 3) {
        len = JANOS_TX_MAX - 3;
    }
    char buf[JANOS_TX_MAX];
    memcpy(buf, line, len);
    buf[len++] = '\r';
    buf[len++] = '\n';

    esp_err_t err = ESP_FAIL;
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    if (s_connected && s_dev) {
        err = cdc_acm_host_data_tx_blocking(s_dev, (const uint8_t *)buf, len, 1000);
    }
    xSemaphoreGive(s_dev_mutex);
    return err == ESP_OK;
}

void janos_usb_manager_set_line_callback(janos_line_cb_t cb, void *user_arg)
{
    s_line_cb = cb;
    s_line_arg = user_arg;
}

/* The band field is the last quoted CSV token, e.g. ...,"5GHz". Comparing the
 * final "..." token (rather than splitting on ',') tolerates commas in SSIDs. */
static bool janos_line_band_is_5ghz(const char *line)
{
    const char *end = strrchr(line, '"');
    if (!end || end == line) {
        return false;
    }
    const char *start = end - 1;
    while (start > line && *start != '"') {
        start--;
    }
    if (*start != '"') {
        return false;
    }
    size_t n = (size_t)(end - (start + 1));
    return (n == 4 && strncmp(start + 1, "5GHz", 4) == 0);
}

bool janos_usb_scan_networks_5ghz(void)
{
    if (!s_connected) {
        glog("C5: not connected. Attach the JanOS C5 to the Tab5 USB-A port.\n");
        return false;
    }

    /* Arm the collector. */
    xSemaphoreTake(s_collect_mutex, portMAX_DELAY);
    s_collect_len = 0;
    s_collect_buf[0] = '\0';
    strncpy(s_collect_marker, "Scan results printed", sizeof(s_collect_marker) - 1);
    s_collect_marker[sizeof(s_collect_marker) - 1] = '\0';
    s_collecting = true;
    xSemaphoreGive(s_collect_mutex);
    xSemaphoreTake(s_collect_sem, 0); /* drain any stale signal */

    if (!janos_usb_manager_send_line("scan_networks")) {
        xSemaphoreTake(s_collect_mutex, portMAX_DELAY);
        s_collecting = false;
        xSemaphoreGive(s_collect_mutex);
        glog("C5: failed to send scan_networks\n");
        return false;
    }
    glog("C5: scanning all channels (this can take a few seconds)...\n");

    bool got = (xSemaphoreTake(s_collect_sem, pdMS_TO_TICKS(JANOS_SCAN_TIMEOUT_MS)) == pdTRUE);

    /* Disarm; after this the RX callback no longer touches s_collect_buf, so it
     * is safe to parse without holding the lock. */
    xSemaphoreTake(s_collect_mutex, portMAX_DELAY);
    s_collecting = false;
    xSemaphoreGive(s_collect_mutex);

    if (!got) {
        glog("C5: scan timed out after %d s\n", JANOS_SCAN_TIMEOUT_MS / 1000);
        return false;
    }

    glog("C5 5 GHz networks:\n");
    int count = 0;
    char *save = NULL;
    for (char *ln = strtok_r(s_collect_buf, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        if (janos_line_band_is_5ghz(ln)) {
            glog("  %s\n", ln);
            count++;
        }
    }
    glog("C5: %d 5 GHz network(s) found\n", count);
    return true;
}

#else /* !CONFIG_JANOS_USB */

void janos_usb_manager_init(void) {}
bool janos_usb_manager_is_connected(void) { return false; }
bool janos_usb_manager_send_line(const char *line) { (void)line; return false; }
void janos_usb_manager_set_line_callback(janos_line_cb_t cb, void *user_arg) { (void)cb; (void)user_arg; }
bool janos_usb_scan_networks_5ghz(void) { return false; }

#endif /* CONFIG_JANOS_USB */
