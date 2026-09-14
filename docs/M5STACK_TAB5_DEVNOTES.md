# M5Stack Tab5 + ESP32-C5 (JanOS) — Developer Notes

Engineering notes and hard-won lessons for the Tab5 (ESP32-P4) work: the USB-A
JanOS C5 add-on, automatic 2.4/5 GHz band routing, and the USB-C CLI. Companion
to the user-facing [`M5STACK_TAB5.md`](./M5STACK_TAB5.md); this file is the
"why / how / gotchas" reference. Everything below was validated on real hardware
unless noted.

## Architecture at a glance

- **Tab5 = ESP32-P4** (app, 5" MIPI-DSI UI) **+ onboard ESP32-C6** (radio) over
  ESP-Hosted SDIO. C6 is **2.4 GHz only**.
- **5 GHz** comes from an **external ESP32-C5 running JanOS**, plugged into the
  Tab5's **USB-A host port**. The P4 is the USB host; it speaks JanOS's plain
  text CLI (`\r\n` lines, 115200 8N1) over USB CDC.
- Gated by `CONFIG_JANOS_USB` (default `y` in both Tab5 profiles).

## USB-A transport (the hard part)

The C5 add-on reaches USB-A through a **USB-UART bridge**, not native USB. This
was the central surprise — the original CDC-ACM-only code couldn't talk to it.
`main/managers/janos_usb_manager.c` now handles all three cases:

| Device | VID:PID | Class | Handling |
| --- | --- | --- | --- |
| CH34x (CH343/CH9102) | 0x1A86:0x55D3 | 0x02 (CDC) | Opened directly via `cdc_acm_host` (CDC-compliant, honours SetLineCoding/SetControlLineState) |
| CP210x (CP2102N) | 0x10C4:0xEA60 | 0x00 (vendor) | `cdc_acm_host` opens the bulk endpoints (two-bulk fallback); UART enable + baud + DTR/RTS via **Silabs vendor control requests** |
| Native Espressif USB | 0x303A:0x1001 | CDC | Opened as CDC-ACM |

Key implementation points:

- A **usb_host diagnostic client** logs every USB-A device's VID/PID/class via
  `glog` on connect, and records the VID so the connect path can branch. This is
  also invaluable for diagnosing "what did I plug in".
- Open order: try JanOS native IDs first, then `CDC_HOST_ANY_VID/PID`.
- **CP210x vendor init** (no extra component, no C++ VCP driver — done with
  `cdc_acm_host_send_custom_request`):
  - `IFC_ENABLE` (bReq 0x00, wValue 0x0001) — enable the UART (without this, no
    data flows at all).
  - `SET_LINE_CTL` (bReq 0x03, wValue 0x0800 = 8N1).
  - `SET_BAUDRATE` (bReq 0x1E, 4-byte LE baud payload).
  - `SET_MHS` (bReq 0x07) for DTR/RTS (low byte = states, high byte = mask).
  - All with `bmRequestType = 0x41`.
- **Run-mode reset on connect.** Bridge DTR/RTS are wired to the C5's EN/IO0
  (the esptool auto-reset circuit). Merely opening the port can leave the C5
  held in reset or in **ROM download mode**, so we pulse a run-mode reset: keep
  DTR deasserted (IO0 high = run) and toggle RTS 0→1→0 (EN low then high). CH34x
  uses `cdc_acm_host_set_control_line_state`; CP210x uses `SET_MHS`. Verified:
  after this the C5 boots and streams its console.
- **Reachability self-test:** after the reset, wait ~6 s for JanOS to boot
  (it probes SD/CC1101/NRF24 for several seconds), then `ping` until it answers
  `pong`; log the result. The RX sink streams the C5 console to the terminal.

### Why not the `usb_host_vcp` component?
It's C++ (`esp_usb::VCP`), which is awkward from GhostESP's C code, and
`cdc_acm_host_send_custom_request` already lets us do the CP210x/CH34x vendor
transfers directly. If an **FTDI** (0x0403) or vendor-class **CH340** (0x1A86
class 0xFF) C5 shows up, that's the point to reconsider adding the driver — the
diag client already tags those.

## Command surface — `c5*` (main/core/commands/cmd_janos.c)

- `c5` / `c5raw <command…>` — **universal passthrough** to any of JanOS's ~103
  commands. This alone is "full capability"; the rest are conveniences.
- `c5help` — streams JanOS's own annotated command list (discovery).
- 5 GHz Wi-Fi: `c5scan` (scan + 5 GHz filter), `c5scanall`, `c5results`,
  `c5select`, `c5deauth`, `c5handshake`, `c5sniff`, `c5karma`, `c5beacon`,
  `c5stop`.
- Recon/radios/creds: `c5hosts`, `c5probes`, `c5bt`, `c5airtag`, `c5wardrive`,
  `c5pass`. Status: `c5status`, `c5version`, `c5ping`, `c5reboot`.
- Output streams to the terminal via the manager's default line sink.

## Automatic band routing (2.4 GHz C6 + 5 GHz C5)

Goal: one AP scan → one list spanning both bands, auto-enabled when a C5 is
attached.

- `janos_usb_scan_collect(cb, ctx, only_5ghz)` runs a C5 scan and reports each
  parsed AP (ssid/bssid/channel/rssi/security) via callback. Shared
  `janos_run_scan()` + a quote-aware CSV field parser (tolerates commas in
  SSIDs).
- `wifi_manager_merge_janos_5ghz()` (in `wifi_manager.c`) appends the C5's 5 GHz
  APs to `scanned_aps[]` as `wifi_ap_record_t` (security string → `authmode`),
  bounded by `MAX_SCANNED_APS`. **No-op when no C5 is connected**, so it
  self-enables on hotplug and never affects a C5-less Tab5.
- **Hook point:** `cmd_wifi_scan_start` (the `scanap` handler), between the C6
  scan and the results print — so both the printout and `list` show the combined
  set. This covers `scanap` from the **serial CLI, on-screen terminal, and
  WebUI** (all route through the command).
- Verified: `scanap 12` → "Found 11 access points" (C6) then "Merged 6 5 GHz
  AP(s) from the C5 (total 17 APs)"; `list` shows channels 3/11 (2.4 GHz) with
  153 (5 GHz), OUI vendor lookup on both.

### NOT yet covered (TODO)
The on-screen menu's **"Scan APs Live"** (continuous 2.4 GHz monitor-mode
capture — the C5's request/response CLI can't feed a live stream) and the cold
**"List APs"** async scan (`ap_scan_start_async()` + LVGL poll timer in
`options_screen.c`, finalized in `ap_scan_finish_async()`). Merging there safely
needs a **background merge task** (the C5 scan is ~15 s blocking; running it on
the LVGL timer thread freezes the UI) plus poll-timer coordination so the UI
doesn't read `scanned_aps` while the merge reallocs it. This is UI work that
wants visual verification. Interim: after any `scanap`, the on-screen "List APs"
shows the merged cached results (it just displays `scanned_aps`).

## Enabling the USB-C CLI on ESP32-P4

By default the P4 read its CLI **only from UART0 pins** — `JTAG_SUPPORTED` in
`main/core/serial_manager.c` excluded P4, so nothing could invoke commands over
the USB-C port. Added `CONFIG_IDF_TARGET_ESP32P4` to `JTAG_SUPPORTED` (matching
S3/C3/C5/C6), which enables `usb_serial_jtag` read/write for the interactive CLI
over USB-C. Log **output** already worked via the secondary console (printf);
this adds **input**. NOTE: this also changes the **CrowPanel Advanced P4** binary
(adds a USB-C CLI) — additive and matching other targets, but not yet tested on
CrowPanel P4 hardware.

## Command registry sizing

`COMMAND_REGISTRY_MAX` (main/core/commandline.c) was **192**; the Tab5's active
command set (base + P4/Tab5 + the `c5*` verbs) exceeded it, so later commands
(e.g. `subghz`) failed with **"command registry full"** and the device
boot-looped. Raised to **256** (SPIRAM-backed pool, ~cheap). If you add more
commands for the largest profile, watch this ceiling.

## Build / flash / serial procedures

**Toolchain:** ESP-IDF **v6.1** for the P4/Tab5 (not v5.4.1):
```bash
source ~/.espressif/v6.1/esp-idf/export.sh
cp configs/sdkconfig.m5stack_tab5_landscape sdkconfig.defaults   # or the portrait profile
rm -f sdkconfig
idf.py set-target esp32p4      # fetches usb_host_cdc_acm etc.
idf.py build
```
> **GOTCHA — stale `sdkconfig.defaults`:** `set-target`/`reconfigure` read
> `sdkconfig.defaults`, NOT `configs/*`. Editing a profile in `configs/` does
> nothing until you re-copy it to `sdkconfig.defaults` and `rm -f sdkconfig`.
> Symptom: a feature compiles but is silently absent (its Kconfig option was
> never set). Always verify:
> `grep JANOS build/config/sdkconfig` and
> `strings build/Ghost_ESP_IDF.bin | grep c5scan` before flashing.

**Flash (over USB-C, esptool via USB-Serial-JTAG):**
```bash
# App only (fast iteration; bootloader/parttable/slave_fw unchanged):
python -m esptool --chip esp32p4 -p /dev/ttyACM0 -b 460800 --before default-reset \
    --after hard-reset write-flash 0x10000 build/Ghost_ESP_IDF.bin
# Full image (own partition table + C6 slave_fw; needed for the C6 radio):
python3 scripts/package_tab5_launcher.py --build-dir build --out local_builds --name GhostESP-Tab5-Landscape
python -m esptool --chip esp32p4 -p /dev/ttyACM0 -b 460800 --before default-reset \
    --after hard-reset write-flash 0x0 local_builds/GhostESP-Tab5-Landscape.bin
```

**Serial capture — gotchas:**
- The P4 native USB-Serial-JTAG only flows data when the host **asserts DTR**.
  Use `esp-idf-monitor` (`idf.py -p /dev/ttyACM0 monitor`), or pyserial with
  `dtr=True`. A plain pyserial open with `dtr=False` gets **silence**.
- The **Launcher** uses a different USB stack and responds to pyserial without
  DTR — handy for telling "am I on the Launcher or GhostESP" (the Launcher CLI
  has `nav`/`partitions`/`flash firmware`; GhostESP has `version`/`help`/`c5*`).
- The Tab5 USB-CDC **re-enumerates on reset**; re-detect the port after flashing.
- INFO-level logs are suppressed after boot: `main.c` calls
  `esp_log_level_set("*", settings_log_level)` (~line 926), so `ESP_LOGI` after
  that is hidden. Use `glog`/`printf` (always visible) for anything you need to
  see in a boot capture. This is why the JanOS status lines use `glog`.

## Launcher (M5StackLauncher) install/reinstall

- Source: `~/Repos/Launcher` (PlatformIO, `m5stack-tab5` env). Prebuilt merged
  image at `~/Repos/Launcher/Launcher-m5stack-tab5.bin` (0x0-origin:
  bootloader@0x2000, parttable@0x8000, app@0x10000). Flash at `0x0`.
- **SD install of GhostESP via the Launcher:** the Launcher's SD picker scans
  the **card root** and lists folders + `*.BIN`; navigate into a subfolder
  (e.g. `/firmware`) if that's where the bin is. `updateFromSD` parses the
  merged image's embedded partition table (magic `AA 50 01` at 0x8000),
  repartitions, and installs the app **plus** data partitions of subtype
  0x81/0x82/0x83 — which is why GhostESP's `slave_fw` (C6 image) uses SPIFFS
  subtype **0x82** (`partitions_m5stack_tab5.csv`), so the Launcher installs it.
- Serial `flash firmware <name> <size>` is the WRONG tool for the merged image —
  it streams a single app over serial with no data partitions (no `slave_fw`).
  Use the SD-menu path.

## Hardware findings / gotchas log

- The C5 add-on that reaches the Tab5 USB-A is a **USB-UART bridge** (CH343 or
  CP2102N), not native USB. Confirm the variant from the boot log's
  "USB-A device: VID=… PID=…" line.
- Two C5 boards were in play: a **CH343** board running **stock Arduino firmware**
  (Bruce-like: `theme.cpp`/`sd_diskio`/`esp32-hal` in its boot log — NOT JanOS),
  and a **CP2102N** board running **JanOS 1.7.1**. JanOS is **ESP-IDF**
  (`~/Repos/JanOS/ESP32C5`, `cmd_scan_networks`, logs like `I (123) TAG:`), so
  Arduino-style `[E][file.cpp:line]` logs mean it's *not* JanOS.
- You cannot flash the C5 through the Tab5 (it's downstream of the P4 USB host,
  not on the dev machine's USB). To flash JanOS, connect the C5 directly to the
  host PC (it appears as `/dev/ttyUSB0` for CP2102N, `/dev/ttyACM*` for CH343).
- JanOS scan: `scan_networks` runs a background dual-band sweep (~13-20 s),
  auto-prints CSV rows, and ends with **"Scan results printed."** (the collect
  marker). CSV: `"idx","ssid","","bssid","channel","security","rssi","band"`;
  band is `≤ch14 ? 2.4GHz : 5GHz`. `ping` → `pong`. JanOS command reference:
  `~/Repos/JanOS/ESP32C5/SKILL/commands-reference.md`.
- A 5 s C6 scan can return 0 APs in a sparse 2.4 GHz environment; `scanap 12`
  (longer) finds them. The C6 radio itself is fine.

## File map (what changed)

- `main/managers/janos_usb_manager.c` / `include/managers/janos_usb_manager.h` —
  USB host + CDC/CP210x/CH34x transport, run-mode reset, ping self-test,
  `janos_usb_scan_collect`.
- `main/core/commands/cmd_janos.c` — the `c5*` command handlers.
- `include/core/commands.h`, `main/core/commandline.c` — prototypes +
  registration (guarded by `CONFIG_JANOS_USB`); `COMMAND_REGISTRY_MAX` 192→256.
- `main/managers/wifi_manager.c` / `include/managers/wifi_manager.h` —
  `wifi_manager_merge_janos_5ghz()`.
- `main/core/commands/cmd_wifi.c` — merge hook in `cmd_wifi_scan_start`.
- `main/core/serial_manager.c` — `JTAG_SUPPORTED` includes ESP32-P4 (USB-C CLI).
- `main/Kconfig.projbuild`, `main/idf_component.yml`, `main/CMakeLists.txt`,
  `configs/sdkconfig.m5stack_tab5*` — `CONFIG_JANOS_USB`, `usb_host_cdc_acm`.
- `docs/M5STACK_TAB5.md` — user-facing docs.

## Commits (branch `tab5-port`)

- `f8e5d77f` — initial JanOS USB-A add-on (CDC-ACM assumption).
- `b2a3339e` — CH34x/CP210x bridge support + reachability (ping/pong) verified.
- `9dea084f` — full `c5*` command surface + P4 USB-C CLI.
- `eba3687f` — automatic 2.4 + 5 GHz band routing; `COMMAND_REGISTRY_MAX`→256.
- `02b28318` — docs.

## Open TODOs

1. On-screen async "List APs" cold-scan + "Scan APs Live" dual-band merge
   (background merge task + poll coordination; needs on-screen visual testing).
2. Confirm the `JTAG_SUPPORTED`-on-P4 change on **CrowPanel Advanced P4** hardware.
3. If a bridge-less/native or FTDI/CH340-vendor C5 appears, revisit the driver
   path (diag client already identifies them).
4. Optional deeper integration: band-aware attack routing (5 GHz targets → C5).
