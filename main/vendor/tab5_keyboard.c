#include "sdkconfig.h"
#include "vendor/drivers/tab5_keyboard.h"

#ifdef CONFIG_M5STACK_TAB5_KEYBOARD

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2c_master.h"
#include "managers/display_manager.h"
#include "esp_log.h"
#include "i2c_shared.h"

/*
 * M5Stack Tab5 keyboard I2C protocol (official spec, addr 0x6D):
 *   0x02 EVENT_NUM   queue length in the active mode (auto-decrements per event read)
 *   0x10 MODE        0=Normal, 1=HID, 2=Character   (we use HID)
 *   0x30 HID_EVENT   2 bytes {modifier, keycode}; empty queue -> 0xFF; release -> {0,0}
 *   0xF0 VERSION
 * HID keycodes are standard USB HID usage IDs; modifier byte is the standard
 * HID modifier bitmap (bit1/bit5 = Left/Right Shift).
 */
#define TAB5_KB_ADDR 0x6D
#define TAB5_KB_I2C_PORT 1 /* I2C_NUM_1 (separate controller from the port-0 touch bus) */
#define TAB5_KB_SDA 0      /* GPIO0 (J9) */
#define TAB5_KB_SCL 1      /* GPIO1 (J9) */
#define TAB5_KB_HZ 400000

#define TAB5_KB_REG_EVENT_NUM 0x02
#define TAB5_KB_REG_MODE 0x10
#define TAB5_KB_REG_HID_EVENT 0x30
#define TAB5_KB_MODE_HID 1

#define TAB5_KB_HID_MOD_SHIFT 0x22 /* left(bit1) | right(bit5) shift */

/* LVGL v8 navigation key codes (match the values GhostESP views expect). */
#define TAB5_KEY_ENTER 13
#define TAB5_KEY_ESC 27
#define TAB5_KEY_BACKSPACE 8
#define TAB5_KEY_TAB 9
#define TAB5_KEY_DEL 127
#define TAB5_KEY_UP 17    /* LV_KEY_UP */
#define TAB5_KEY_DOWN 18  /* LV_KEY_DOWN */
#define TAB5_KEY_RIGHT 19 /* LV_KEY_RIGHT */
#define TAB5_KEY_LEFT 20  /* LV_KEY_LEFT */

static const char *TAG = "tab5_kbd";
static i2c_master_bus_handle_t s_kb_bus = NULL;
static TaskHandle_t s_kb_task = NULL;
static uint8_t s_held = 0;      /* HID keycode currently held (0 = none), for edge detection */
static uint8_t s_held_char = 0; /* keyboard char emitted for the held key (0 if nav/none) */
static bool s_kb_present = false;

bool tab5_keyboard_present(void)
{
    return s_kb_present;
}

/* Shifted symbols for the US number row 1..0 (HID 0x1E..0x27). */
static const char k_num_shift[10] = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'};

static esp_err_t kb_read(uint8_t reg, uint8_t *out, size_t len)
{
    if (!s_kb_bus) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_shared_transmit_receive_from_addr(s_kb_bus, TAB5_KB_ADDR, TAB5_KB_HZ, &reg, 1, out, len, 100);
}

static esp_err_t kb_write(uint8_t reg, uint8_t val)
{
    if (!s_kb_bus) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t payload[2] = {reg, val};
    return i2c_shared_transmit_to_addr(s_kb_bus, TAB5_KB_ADDR, TAB5_KB_HZ, payload, sizeof(payload), 100);
}

/* Translate a HID usage code + modifier to a GhostESP key_value (0 = ignore). */
static uint8_t tab5_hid_to_key(uint8_t mod, uint8_t kc)
{
    bool shift = (mod & TAB5_KB_HID_MOD_SHIFT) != 0;

    if (kc >= 0x04 && kc <= 0x1D) { /* a..z */
        char c = (char)('a' + (kc - 0x04));
        return shift ? (uint8_t)(c - ('a' - 'A')) : (uint8_t)c;
    }
    if (kc >= 0x1E && kc <= 0x27) { /* 1..9,0 */
        int idx = kc - 0x1E; /* 0..9 -> 1,2,..,9,0 */
        if (shift) {
            return (uint8_t)k_num_shift[idx];
        }
        return (uint8_t)((idx == 9) ? '0' : ('1' + idx));
    }
    switch (kc) {
    case 0x28: return TAB5_KEY_ENTER;
    case 0x29: return TAB5_KEY_ESC;
    case 0x2A: return TAB5_KEY_BACKSPACE;
    case 0x2B: return TAB5_KEY_TAB;
    case 0x2C: return (uint8_t)' ';
    case 0x2D: return shift ? (uint8_t)'_' : (uint8_t)'-';
    case 0x2E: return shift ? (uint8_t)'+' : (uint8_t)'=';
    case 0x2F: return shift ? (uint8_t)'{' : (uint8_t)'[';
    case 0x30: return shift ? (uint8_t)'}' : (uint8_t)']';
    case 0x31: return shift ? (uint8_t)'|' : (uint8_t)'\\';
    case 0x33: return shift ? (uint8_t)':' : (uint8_t)';';
    case 0x34: return shift ? (uint8_t)'"' : (uint8_t)'\'';
    case 0x35: return shift ? (uint8_t)'~' : (uint8_t)'`';
    case 0x36: return shift ? (uint8_t)'<' : (uint8_t)',';
    case 0x37: return shift ? (uint8_t)'>' : (uint8_t)'.';
    case 0x38: return shift ? (uint8_t)'?' : (uint8_t)'/';
    case 0x4C: return TAB5_KEY_DEL;   /* Delete */
    case 0x4F: return TAB5_KEY_RIGHT;
    case 0x50: return TAB5_KEY_LEFT;
    case 0x51: return TAB5_KEY_DOWN;
    case 0x52: return TAB5_KEY_UP;
    default: return 0;
    }
}

/* Navigation keys are delivered as joystick events (like usb_keyboard_manager),
 * because far more GhostESP views handle joystick nav than keyboard arrow codes,
 * and joystick events are unambiguous (they never type). Indices: LEFT=0,
 * ENTER=1, UP=2, RIGHT=3, DOWN=4. ESC maps to LEFT(0) because GhostESP's back
 * action IS a joystick-left delivered to the active view (the same event the
 * left-edge swipe sends); the view decides its own one-step back. */
static int tab5_hid_to_joystick(uint8_t kc)
{
    switch (kc) {
    case 0x50: return 0; /* LEFT  */
    case 0x28: return 1; /* ENTER / select */
    case 0x52: return 2; /* UP    */
    case 0x4F: return 3; /* RIGHT */
    case 0x51: return 4; /* DOWN  */
    case 0x29: return 0; /* ESC -> back (== left-edge swipe) */
    default:   return -1;
    }
}

static void tab5_kb_emit_key(uint8_t key_value, bool pressed)
{
    if (pressed && display_manager_notify_user_input()) {
        return; /* consumed as a wake-from-dim event */
    }
    InputEvent ev = {0};
    ev.type = INPUT_TYPE_KEYBOARD;
    ev.data.key_value = key_value;
    ev.is_touch_move = !pressed; /* release flag: stops the global key-repeat timer */
    xQueueSend((QueueHandle_t)input_queue, &ev, 0);
}

static void tab5_kb_emit_joystick(int index)
{
    if (display_manager_notify_user_input()) {
        return; /* consumed as a wake-from-dim event */
    }
    InputEvent ev = {0};
    ev.type = INPUT_TYPE_JOYSTICK;
    ev.data.joystick_index = index;
    ev.data.joystick_pressed = true;
    xQueueSend((QueueHandle_t)input_queue, &ev, 0);
}

static void tab5_keyboard_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15));
        uint8_t count = 0;
        if (kb_read(TAB5_KB_REG_EVENT_NUM, &count, 1) != ESP_OK) {
            continue;
        }
        count &= 0x3F; /* 0..32 */
        while (count-- > 0) {
            uint8_t ev[2] = {0xFF, 0xFF};
            if (kb_read(TAB5_KB_REG_HID_EVENT, ev, 2) != ESP_OK) {
                break;
            }
            if (ev[0] == 0xFF && ev[1] == 0xFF) {
                break; /* queue drained */
            }
            uint8_t keycode = ev[1];
            if (keycode == 0) {
                /* all-zero report = all keys released. Emit a keyboard release for
                 * the held char (stops the host key-repeat); nav keys are one-shot. */
                if (s_held_char) {
                    tab5_kb_emit_key(s_held_char, false);
                    s_held_char = 0;
                }
                s_held = 0;
                continue;
            }
            if (keycode == s_held) {
                /* The keyboard re-reports a held key every poll; emit only on the
                 * press edge. The host (kb_repeat) handles auto-repeat for typing. */
                continue;
            }
            s_held = keycode;
            int joy = tab5_hid_to_joystick(keycode);
            if (joy >= 0) {
                tab5_kb_emit_joystick(joy); /* one-shot nav (arrows/enter/esc) */
                s_held_char = 0;
                continue;
            }
            uint8_t key_value = tab5_hid_to_key(ev[0], keycode);
            if (key_value) {
                tab5_kb_emit_key(key_value, true);
                s_held_char = key_value;
            } else {
                s_held_char = 0;
            }
        }
    }
}

void tab5_keyboard_init(void)
{
    bool created = false;
    if (i2c_shared_get_or_create_bus(TAB5_KB_I2C_PORT, TAB5_KB_SDA, TAB5_KB_SCL, true, &s_kb_bus, &created) != ESP_OK) {
        ESP_LOGW(TAG, "Tab5 keyboard I2C bus init failed (port %d, SDA %d, SCL %d)",
                 TAB5_KB_I2C_PORT, TAB5_KB_SDA, TAB5_KB_SCL);
        s_kb_bus = NULL;
        return;
    }

    /* Probe + put the keyboard into HID event-queue mode. If it is absent the
     * write fails and we skip starting the poll task (keyboard is optional). */
    if (kb_write(TAB5_KB_REG_MODE, TAB5_KB_MODE_HID) != ESP_OK) {
        ESP_LOGI(TAG, "No Tab5 keyboard detected at 0x%02X (optional)", TAB5_KB_ADDR);
        return;
    }

    s_kb_present = true;
    uint8_t ver = 0;
    if (kb_read(0xF0, &ver, 1) == ESP_OK) {
        ESP_LOGI(TAG, "Tab5 keyboard ready (HID mode, fw 0x%02X)", ver);
    } else {
        ESP_LOGI(TAG, "Tab5 keyboard ready (HID mode)");
    }

    if (!s_kb_task) {
        xTaskCreate(tab5_keyboard_task, "tab5_kbd", 3072, NULL, HARDWARE_INPUT_TASK_PRIORITY + 1, &s_kb_task);
    }
}

#endif /* CONFIG_M5STACK_TAB5_KEYBOARD */
