# GhostESP on the M5Stack Tab5

The **M5Stack Tab5** is an **ESP32-P4** host (application, 5-inch MIPI-DSI display,
UI) with an **onboard ESP32-C6** that provides the radio over ESP-Hosted SDIO.
That is the same architecture GhostESP already ships for the **CrowPanel
Advanced P4** boards, including the custom "ghost raw-radio" C6 slave image
(`firmware/crowpanel_p4/network_adapter.bin`) that carries monitor mode / raw
802.11 injection over ESP-Hosted. So the Tab5 runs the full GhostESP feature set
on 2.4 GHz Wi-Fi + BLE with no radio rewrite — this is a board bring-up, not a
port of the radio stack.

## What works

- **2.4 GHz Wi-Fi + BLE** via the onboard C6 (ESP-Hosted SDIO), including the
  raw-radio features (deauth, sniff, Evil Portal, PMKID/handshake, etc.).
- **Display**, with **automatic panel-variant detection** at boot: early units
  (ILI9881C + GT911 touch) and the newer 2026-04-28+ units (ST7123 / ST7121 +
  touch @0x55) are both driven, all 720×1280 two-lane MIPI-DSI. GPIO22 LEDC
  backlight, power/reset through the two PI4IOE5V6408 I/O-expanders.
- **Landscape mode** (optional, `CONFIG_M5STACK_TAB5_LANDSCAPE`): the UI renders
  at 1280×720 and is rotated 90° onto the portrait panel by the ESP32-P4 **PPA**
  hardware engine, with touch coordinates transformed to match.
- **M5Stack Tab5 keyboard** (M5Unit-KEYBOARD on J9): text entry + menu navigation
  (arrows / Enter / Esc-as-back). In landscape the on-screen keyboard is
  suppressed when the physical keyboard is attached.
- **microSD**, Cloud Store, WebUI, serial CLI, GhostScript — as on other boards.
- **5 GHz** by attaching an external **ESP32-C5**: plug a JanOS C5 into the
  **USB-A** port (`c5scan`, `c5deauth`, `c5raw …`), or wire a GhostLink UART peer
  (see below).

## Hardware map (ESP32-P4 host)

| Function | Pins / value |
| --- | --- |
| MIPI-DSI panel | 720×1280, 2 data lanes, D-PHY on LDO ch3 @2.5 V. ILI9881C @730 Mbps/60 MHz, or ST7123/ST7121 @965 Mbps/70 MHz (auto-detected) |
| Backlight | GPIO22, LEDC PWM |
| Touch | system I2C (SDA 31 / SCL 32): GT911 @0x5D (ILI9881C units) or ST7123/ST7121 @0x55 (newer units); INT GPIO23, reset via expander |
| Keyboard (M5Unit-KEYBOARD) | J9 connector on **I2C_NUM_1**: SDA GPIO0, SCL GPIO1, INT GPIO50, addr 0x6D (HID mode) |
| I/O expanders | PI4IOE5V6408 @0x43 (LCD_RST P4, TP_RST P5) and @0x44 (**WLAN_PWR_EN P0** = C6 radio power) |
| C6 over ESP-Hosted SDIO | slot 1, 4-bit: CLK 12, CMD 13, D0 11, D1 10, D2 9, D3 8; slave reset GPIO15 |
| microSD (SDMMC slot 0) | CLK 43, CMD 44, D0 39; SD rail on LDO ch4 @3.3 V |

The C6 radio rail (`WLAN_PWR_EN`) is asserted early in `app_main`, before
`esp_hosted_init()`, or the C6 never enumerates. The onboard C6 is **2.4 GHz
only**. The keyboard maps navigation keys to joystick events (arrows, Enter, and
Esc→joystick-left = the same one-step "back" as a left-edge swipe) and printable
keys to characters — mirroring GhostESP's USB-keyboard input path.

## Building

Two board profiles (portrait is the default; landscape adds the PPA rotation):

```bash
source ~/.espressif/v6.1/esp-idf/export.sh          # ESP-IDF v6.1 (P4)
# via the build script (recommended — bundles the C6 image + merges):
python3 build.py --targets <index-of "M5Stack Tab5">              # portrait 720×1280
python3 build.py --targets <index-of "M5Stack Tab5 (Landscape)">  # landscape 1280×720
# or directly (swap in configs/sdkconfig.m5stack_tab5_landscape for landscape):
cp configs/sdkconfig.m5stack_tab5 sdkconfig.defaults && rm -f sdkconfig
idf.py set-target esp32p4 && idf.py build
```

The C6 ESP-Hosted image is bundled into the `slave_fw` flash partition
automatically and pushed to the C6 over SDIO on first boot (no separate C6
flash step needed). The keyboard is included in both profiles by default.

## Flashing

Package the built image (works for both direct flash and the Launcher):

```bash
python3 scripts/package_tab5_launcher.py --build-dir build --out local_builds \
        --name GhostESP-Tab5-Landscape        # or GhostESP-Tab5-Portrait
# Direct flash:
esptool --chip esp32p4 write-flash 0x0 local_builds/GhostESP-Tab5-Landscape.bin
```

`idf.py flash` also works (bootloader @0x2000, part table @0x8000, app @0x10000,
C6 image @0xd90000).

**Via the [M5StackLauncher](https://github.com/joshuawowk/M5StackLauncher):**

1. Copy the packaged `.bin` to the SD card.
2. Boot the Launcher → **SD Card** → select the file.
3. The Launcher repartitions, installs the app + the C6 radio image, and reboots
   into GhostESP.

The packaged image is byte-identical to what `idf.py flash` writes, so it serves
both the direct flash and the Launcher install. The `slave_fw` partition uses the
SPIFFS data subtype (0x82) specifically so the Launcher creates and populates it
(the C6 radio image) — GhostESP locates it by name, so nothing mounts it as a
filesystem. Validate any image with:

```bash
python3 scripts/package_tab5_launcher.py --check local_builds/GhostESP-Tab5-Landscape.bin
```

(SD-menu display names are truncated to 20 characters by the Launcher — cosmetic;
the install is unaffected.)

## Adding 5 GHz with a USB-A ESP32-C5 running JanOS (`CONFIG_JANOS_USB`)

The Tab5's USB-A port is an ESP32-P4 USB 2.0 **host** port (VBUS enabled by the
board power init). An external ESP32-C5 running **JanOS** — plugged into USB-A —
gives 5 GHz coverage without any wiring: the P4 acts as a USB **CDC-ACM host**
and speaks JanOS's plain-text CLI (`\r\n` lines, 115200 8N1). Enabled by default
in both Tab5 profiles (`CONFIG_JANOS_USB=y`).

1. Flash JanOS onto the C5 (`~/Repos/JanOS`, ESP-IDF v6.1). Its native USB endpoint
   must stay a CDC-ACM console — keep `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`
   in the JanOS build (VID `0x303A`, PID `0x1001`). A C5 devkit that reaches USB
   through a CP2102N/CH343 **bridge** instead of native USB is not CDC-ACM and
   would need the `usb_host_vcp` driver — use a native-USB C5.
2. Plug the C5 into the Tab5 USB-A port. GhostESP opens it on hot-plug; the
   terminal prints `C5 (JanOS) connected on USB-A (115200 8N1)`.
3. Drive it from the serial CLI / terminal view:
   - `c5scan` — run a scan and print only the **5 GHz** networks found.
   - `c5results`, `c5select <idx…>`, `c5deauth`, `c5sniff`, `c5stop`,
     `c5hosts`, `c5pass [portal|evil]`, `c5ping` — mapped JanOS verbs.
   - `c5raw <text…>` — send any JanOS command verbatim; its output streams back.

   JanOS indices are 1-based; `c5stop` is the universal cancel. Local 2.4 GHz
   keeps running on the onboard C6 the whole time.

This is the recommended 5 GHz add-on (no wiring, keeps Grove Port A free). The
GhostLink UART path below remains available for a headless GhostESP-on-C5 peer.

## Adding 5 GHz with an external ESP32-C5 (GhostLink UART)

The onboard C6 cannot do 5 GHz. To add it, run GhostESP on an external ESP32-C5
as a GhostLink **radio peer**; the Tab5 stays the display/UI "core" and relays
5 GHz commands to the C5.

1. Flash the C5 with a headless GhostESP C5 profile — e.g.
   `configs/sdkconfig.xiao_esp32c5` (5 GHz enabled, no screen).
2. Wire a 3-wire UART between the Tab5's **Grove Port A** (GPIO53/54) and the
   C5, crossed: Tab5 TX(53)→C5 RX, Tab5 RX(54)→C5 TX, GND↔GND. (3.3 V logic.)
3. On the Tab5: `commsetpins 53 54`; on the C5: `commsetpins <tx> <rx>` for the
   pads you wired. Both default to 115200 baud (no rebuild needed).
4. Pair via the GhostLink view / `commdiscovery` → `commconnect ESP_xxxxxx`, then
   run 5 GHz work on the C5 with `commsend scanap`, `commsend aerial scan`, etc.
   Local 2.4 GHz keeps running on the onboard C6.

Using Grove Port A as UART forfeits its I2C function. An automatic band-aware
split (2.4 → C6, 5 → C5) is not built yet; today the C5 is targeted manually via
`commsend`.

## Validated on hardware

Bring-up was verified on a Tab5 (ESP32-P4 rev v1.3): C6 Wi-Fi + BLE enumerate
over ESP-Hosted, the ST7121/ST7123 panel and touch work, landscape orientation +
touch mapping are correct, and the M5Unit-KEYBOARD drives typing and navigation
(arrows / Enter / Esc-as-back).

Notes and remaining edges:

- **Panel variant is auto-detected** at boot (GT911 @0x14/0x5D → ILI9881C;
  touch @0x55 → ST7121 vs ST7123 by touch-FW version). No build-time choice
  needed for the common units.
- **Landscape rotation angle.** Defaults to 90° (`TAB5_LANDSCAPE_ANGLE_90` in
  `tab5_display.c`); set it to 0 for 270° if a unit is mounted the other way.
- **C6 first-boot.** The bundled 2.12.16 C6 image is OTA-pushed from the P4's
  `slave_fw` partition. If a factory C6 will not handshake, a one-time C6 seed
  flash may be needed once; steady state is maintained by the P4-side OTA. Reset
  polarity is `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH` (A/B with `_ACTIVE_LOW`
  only if the C6 never enumerates).
- **Launcher bootloader.** A Launcher install runs the app under the Launcher's
  own bootloader (not the one in the image). If it hangs right after a Launcher
  install, flash the packaged `.bin` directly at 0x0 to isolate a bootloader/PSRAM
  config difference.
- **The Tab5 USB-CDC re-enumerates on reset**, so re-detect the serial port
  before flashing.

Not enabled yet: onboard camera (SC2356), audio (ES8388/ES7210), and automatic
2.4/5 GHz band routing to a C5 peer — each a future add-on behind its own option.
