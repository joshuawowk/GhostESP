# M5Stack Tab5 port — reverse-engineering review

A reverse-engineering-led quality review of the GhostESP **M5Stack Tab5** (ESP32-P4) port
(`tab5-port` branch), done to assess the port's quality and to make it a well-behaved
firmware under the [Sentinel Bootloader](https://github.com/joshuawowk/sentinel-bootloader)
launcher. For how GhostESP runs under Sentinel see [`SENTINEL.md`](SENTINEL.md); for the
port's design notes see [`M5STACK_TAB5_DEVNOTES.md`](M5STACK_TAB5_DEVNOTES.md).

## Verdict

The port is **solid**. The hardest and most error-prone parts — the remote-C6 Wi-Fi path,
the USB-A/JanOS bridge handling, the SDMMC host shared with the C6, and the P4 cache/SDIO
configuration — are done correctly, and several traps that bit sibling Tab5 ports are already
avoided here. The review found **no crash or memory-safety defect in the shipped image**. It
did find a handful of latent correctness bugs and robustness gaps, plus the Sentinel-launcher
integration that was missing; the safe, high-value ones are fixed on this branch and the rest
are recorded below with a recommended fix and whether they need on-hardware testing.

## Changes landed (PRs into `tab5-port`)

| PR | Scope | Verified |
|---|---|---|
| #1 | Gentle promiscuous filters + 250 ms hop for the remote C6 radio (airspace monitor, packet monitor) | airspace: hardware; packet-mon: build |
| #2 | Sentinel integration: return-to-launcher (menu + `reboot launcher` CLI), single-slot OTA guard, `SENTINEL_APP_INFO` | build (Tab5) + 16-target CI |
| #3 | Display init robustness (LDO leak, framebuffer-get check, clear-before-backlight, flip-on-success) + keyboard HID-mode retry | build (Tab5) |
| #4 | Robust JanOS bridge detection (CP2102N vs CDC) + dedup of the 5 GHz merge | build (Tab5); JanOS needs a hardware pass |

## Method and evidence

The firmware is the user's own source tree, so this is not a black-box RE. The RE value was in
checking **what the shipped artifact actually contains and links** against the source and
against the Sentinel launcher's documented expectations:

- **Built artifact** (IDF v6.1, `esp32p4`, landscape): `build/Ghost_ESP_IDF.elf`,
  `build/Ghost_ESP_IDF.bin` (4.03 MB app), `local_builds/GhostESP-Tab5-Landscape*.bin`
  (15.5 MB merged), and the bundled C6 image `firmware/crowpanel_p4/network_adapter.bin`.
- **REA** (`inspect_firmware_regions`, binwalk 3.1.0) on the merged image: confirmed the
  embedded WebUI asset (`ghost_site.html`, gzip) and crypto tables; it does not parse the ESP
  partition table, so partition facts come from esptool/the build.
- **IDF rv32 tooling** (REA's native disassembly providers cannot open ESP32-P4 rv32imafc
  ELFs): `riscv32-esp-elf-nm` for linked symbols, `esptool image-info` for the app descriptor,
  and byte reads for the image-format offsets.
- **Static source review** of the port's additions (JanOS USB, display/keyboard drivers,
  Wi-Fi band routing, serial/registry) via focused review agents.
- Cross-referenced against `sentinel-bootloader/docs/{compatibility,firmware-authors,design}.md`.

Confidence is marked **observed** (from the artifact/toolchain), **inferred** (source
reasoning), or **unknown** (needs hardware).

## Observed (binary / toolchain)

- **Partition table shipped**: `nvs`(16K) · `otadata`(8K) · `phy_init`(4K) ·
  `factory`(0x480000) · `ota_0`(0x480000) · `ota_1`(0x480000) · `slave_fw`(0x200000, subtype
  0x82 @0xD90000). Three app slots for a direct flash; under Sentinel only `ota_0` survives,
  plus `nvs`/`phy_init`/`slave_fw`.
- **App descriptor**: project name `Ghost_ESP_IDF`.
- **Bundled C6 image = ESP-Hosted-MCU 2.12.16**, matching the host esp_hosted component
  (2.12.16) and the launcher's expected 2.12.x.
- **OTA/update paths linked**: `ota_manager` (download+raw-write), `self_ota_manager` (hard
  disabled — `is_supported()` returns false), `peer_ota_manager` (GhostLink),
  `p4_slave_ota_manager` (C6). The raw-write path uses `esp_ota_begin()`, which refuses the
  running partition cleanly (no unguarded erase of a running image).
- **Config verified safe against Sentinel's documented traps**: `CACHE_L2_CACHE_LINE_64B=y`
  with `ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` off (avoids the intermittent SDIO buffer-alignment
  wedge a 128 B line + PSRAM mempool causes on IDF ≥ 5.5); `FREERTOS_HZ=1000`; USB-host
  enum-filter callback off; SDIO pins CLK12/CMD13/D0-3 11·10·9·8, C6 reset 15 (the Tab5's pins).

## Sentinel-launcher readiness

GhostESP already **boots** under Sentinel (since Sentinel #13, which restores the UART0 clock
the launcher gated — without it GhostESP's UART0 console hung at `uart_param_config` with an
interrupt-watchdog panic). The three documented gaps are now closed (PR #2):

| Gap (firmware-authors.md) | Before | Now |
|---|---|---|
| Way back to the launcher | none; reset button only | "Return to Launcher" menu item + `reboot launcher` CLI (erase otadata + restart) |
| Self-update under one slot | update item offered, then errors | `ota_manager_is_supported()` requires `next != running`; hidden cleanly |
| In-launcher update source | no `SENTINEL_APP_INFO` | `SNAP` descriptor at 0x120 (repo + per-orientation asset), Tab5-gated |

Already correct and unchanged: the C6 version-compare-then-flash (`p4_slave_ota_manager`), and
the SDMMC no-op host deinit (`sd_card_manager` installs `host.deinit = sdmmc_host_deinit_dummy`,
so unmounting the card never tears down the C6's shared SDMMC host).

## Subsystem findings

### JanOS USB-A bridge (`janos_usb_manager.c`) — good; one latent bug fixed
The three traps that broke the sibling M5MonsterC5 port are **already handled** here: USB-A 5 V
is switched on (`tab5_board_power_init`, expander 0x44 P3) before `usb_host_install`; the CDC
IN buffer is 512 bytes (a multiple of the 64-byte bulk-IN MPS); and line coding is set to
115200 8N1 (and the CP2102N vendor path sets its baud/format).
- **[fixed, PR #4 — needs hardware pass]** CP2102N-vs-CH34x was chosen from a racy global VID
  written by a separate diagnostic client, not from the just-opened device — a CP2102N opened
  before that client processed its event took the CDC path, skipped `cp210x_init`, stayed at
  the wrong baud, and never answered. Now determined by which VID filter opens the device.
- **[recommended]** use-after-free window if the C5 is unplugged between `cdc_acm_host_open`
  and taking `s_dev_mutex`; a disconnect callback that blocks on the mutex while a TX holds it
  (bounded ~1 s stall); serialize `janos_run_scan`.

### Display / keyboard drivers (`tab5_display.c`, `tab5_keyboard.c`) — works; robustness improved
No memory-safety defect found (framebuffer math, PPA bounds, DMA alignment, touch buffer all
check out). The display comes up under Sentinel on a real unit (per compatibility.md), so the
warm-reset issues below are latent. Fixed in PR #3 (safe, no timing changes): MIPI LDO channel
leak on init-error paths; unchecked `esp_lcd_dpi_panel_get_frame_buffer()`; framebuffers
cleared before the backlight turns on; back-buffer index advanced only on a successful present;
keyboard HID-mode write retried 3× (a transient boot NACK used to disable the keyboard).

Recommended, **needs hardware** (not changed, to avoid destabilizing a working panel):
- LCD_RST is never pulsed (relies on the DCS soft reset); after a warm digital-system reset the
  panel keeps the launcher's state. Add a real reset pulse (low ≥10 ms, release, wait ≥120 ms).
- Panel-variant probe guesses ILI9881C on a single failed I2C read → retry before defaulting.
- ST7123 touch controller and the STM32 keyboard's event queue are not reset/flushed on boot
  → stale events can be replayed; modifier-only HID reports are treated as "all released".

### Wi-Fi 5 GHz merge / serial / registry
- **[fixed, PR #4]** `janos_merge_cb` now dedups by BSSID; the merge previously appended the
  C5's 5 GHz APs with no dedup and ran unconditionally, so repeated scans (or a short-circuited
  C6 rescan) grew the list toward the 100-AP cap with duplicate BSSIDs.
- **[known limitation]** the merge runs only on the CLI/WebUI `scan` path, not the on-screen
  menu's async scan, so the touch UI's "List APs" shows 2.4 GHz only (the async LVGL flow needs
  a background-merge + on-screen verification).
- **[pre-existing]** `scanned_aps` is freed/replaced without a lock; the merge widens an
  existing race window shared with the stock scan path. Not introduced by the port.
- **[intended]** adding ESP32-P4 to the serial `JTAG_SUPPORTED` set is target-gated, so the
  CrowPanel Advanced P4 binary now also mirrors the console to USB-Serial-JTAG — intended, not
  yet tested on CrowPanel hardware.
- Command registry raised to 256 is adequate (~228 register sites, bounds-checked, no overflow).

## Residual unknowns / recommendations

- On-hardware validation of the JanOS CP2102N fix (needs a C5 board on this unit's USB-A).
- The display warm-reset robustness items above (panel reset pulse, variant-probe retry, touch
  reset, keyboard queue flush) should be done and tested with the panel in hand.
- A GitHub **release** (with the `GhostESP-Tab5-*.bin` assets) is required before Sentinel's
  in-launcher Update is active; the `SENTINEL_APP_INFO` descriptor is inert until then.
- Pre-existing, unrelated: the repo's `compile_all` CI fails fast on the **Marauder V8** (esp32)
  job with an ESP-IDF `esp_wifi.h` Kconfig-expansion error (`operator '>' has no left operand`),
  which cancels the remaining jobs. 16 targets (incl. esp32/s3/c3/c5/c6 generics) build cleanly
  with these changes; the Marauder V8 failure is in a board config untouched by this work.

## Appendix — key checks

```bash
esptool image-info build/Ghost_ESP_IDF.bin                      # project name / descriptor
python3 -c "import struct;d=open('build/Ghost_ESP_IDF.bin','rb').read();print(struct.unpack_from('<IHH',d,0x120))"  # SENTINEL_APP_INFO 'SNAP'
riscv32-esp-elf-nm build/Ghost_ESP_IDF.elf | grep -E 'sentinel_|ota_manager|_ota_'           # linked symbols
strings firmware/crowpanel_p4/network_adapter.bin | grep -E '2\.12|ESP-Hosted'               # bundled C6 version
```
