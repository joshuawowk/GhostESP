#ifndef M5STACK_TAB5_KEYBOARD_H
#define M5STACK_TAB5_KEYBOARD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * M5Stack Tab5 keyboard (M5Unit-KEYBOARD, STM32F030) on the internal J9
 * connector: I2C 0x6D, SDA=GPIO0, SCL=GPIO1, INT=GPIO50, on I2C_NUM_1 (the
 * system/touch bus is I2C_NUM_0 on GPIO31/32). Runs a poll task that decodes
 * the keyboard's HID event queue and pushes InputEvents onto the display
 * manager's input_queue, exactly like the Cardputer-ADV TCA8418 backend.
 */
void tab5_keyboard_init(void);

/* True once the M5Unit-KEYBOARD has been detected and its poll task started. */
bool tab5_keyboard_present(void);

#ifdef __cplusplus
}
#endif

#endif
