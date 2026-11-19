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
are recorded below with a recommended fix and whether they need on-hardware testing. The
Sentinel integration was validated end-to-end on a brand-new Tab5 — install under Sentinel,
boot, clean display, working touch, SD + Wi-Fi up, and return-to-launcher — see
[Hardware validation](#hardware-validation-new-tab5-sentinel-v016).

## Changes landed (PRs into `tab5-port`)

| PR | Scope | Verified |
|---|---|---|
| #1 | Gentle promiscuous filters + 250 ms hop for the remote C6 radio (airspace monitor, packet monitor) | airspace: hardware; packet-mon: build |
| #2 | Sentinel integration: return-to-launcher (menu + `reboot launcher` CLI), single-slot OTA guard, `SENTINEL_APP_INFO` | build (Tab5) + 16-target CI |
| #3 | Display init robustness (LDO leak, framebuffer-get check, clear-before-backlight, flip-on-success) + keyboard HID-mode retry | build (Tab5) |
| #4 | Robust JanOS bridge detection (CP2102N vs CDC) + dedup of the 5 GHz merge | build (Tab5); JanOS needs a hardware pass |
| #6 | C6 never auto-flashed at boot under a launcher (opt-in `c6ota force` only) | build + hardware (boot-guard log observed) |
| #8 | Scope "unknown C6 → skip" to launcher boots (keep direct-flash provisioning) | build (Tab5) |
| #9 | `version.txt` → PROJECT_VER 2.1.3 (comparable for Sentinel's Update) | build (Tab5) |
| #10 | JanOS: also catch a CP2102N hot-plugged during the ANY-VID open | build (Tab5); JanOS needs a hardware pass |

Docs: #5 (this RE review), #7 (hardware-validation section). Published as release **v2.1.3** (both Tab5 images). REA itself was one `inspect_firmware_regions` (binwalk) pass; the rv32 disassembly limitation was reported by the Tab5 Coordinator, not re-verified here. JanOS, the portrait image, and the packet-monitor filter are **build-verified only** (no C5 board / portrait unit / lockup-repro on hardware).

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
- **IDF rv32 tooling** (REA's native disassembly providers do not open ESP32-P4 rv32imafc
  ELFs — per the Tab5 Coordinator; not independently re-checked here): `riscv32-esp-elf-nm` for
  linked symbols, `esptool image-info` for the app descriptor, and byte reads for the
  image-format offsets.
- **REA** was used for exactly one pass — a single `inspect_firmware_regions` (binwalk 3.1.0)
  scan of the merged image (below); it is not a deep decompilation of the rv32 image.
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
- **[fixed, PR #4 + #10 — needs hardware pass]** CP2102N-vs-CH34x was chosen from a racy global
  VID written by a separate diagnostic client, not from the just-opened device — a CP2102N
  opened before that client processed its event took the CDC path, skipped `cp210x_init`,
  stayed at the wrong baud, and never answered. Now determined by which VID filter opens the
  device, with a fallback to the diagnostic client's VID for a CP2102N hot-plugged during the
  ANY-VID open (PR #10 — the VID-filter-only form in #4 missed that ~3 s window).
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

Recommended, **not changed** (display and touch are confirmed working on hardware — see
"Hardware validation"; these are robustness/cleanup items, not functional defects):
- On the ST7121 unit tested, detect returns **ST712x** (correct): the display renders cleanly
  via the ST712x DSI path and touch works, read directly from the `0x55` controller. The GT911
  I2C errors in the log are **not** a mis-detection — they come from an *unconditional*
  `touch_driver_init()` (display_manager.c:2239) called for every Tab5 regardless of variant,
  harmless on a GT911-less unit. Cleanup opportunities (not functional defects): skip that
  GT911 `touch_driver_init()` on ST712x units to quiet the log, and add a boot-time reset of
  the integrated ST712x controller (GhostESP currently skips it — `tab5_display_touch_reset`
  returns early for ST712x), which also helps the launcher re-detect the panel after GhostESP
  runs (see `panel none` below).
- Keyboard: the STM32 event queue is not flushed on boot and a modifier-only HID report is
  treated as "all released" — stale events can be replayed / a character duplicated.

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

## Hardware validation (new Tab5, Sentinel v0.1.6)

Validated on a brand-new M5Stack Tab5 (MAC `E8:F6:0A:E2:FC:56`, ESP32-P4 **rev v1.3**, 16 MB,
**ST7121** integrated display-touch panel). A full factory backup was read and verified first;
Sentinel Bootloader v0.1.6 (signed release, SHA-256 checked) was factory-installed, then the
guarded GhostESP image was installed and started with `sentinelctl.py`.

| Check | Result |
|---|---|
| Boots under Sentinel | ✅ reaches the UI; no UART0 interrupt-watchdog hang (Sentinel #13 clock restore) |
| Per-firmware view | ✅ `image + nvs + phy_init + slave_fw`, exactly as `compatibility.md` predicted |
| Display | ✅ clean, correct image (user-confirmed) |
| Touch | ✅ responds (read directly from the ST712x `0x55` controller) |
| SD card | ✅ mounts (SDMMC, 14910 MB) |
| Wi-Fi / C6 link | ✅ AP "GhostNet" comes up — the esp-hosted C6 link is functional |
| C6 boot-guard (PR #6) | ✅ no auto-flash at boot; `info` still `c6 unknown` after a GhostESP boot |
| Return to launcher (PR #2) | ✅ `reboot launcher` CLI → boot log shows `warm+armed/launcher+clean` |
| Single-slot OTA guard (PR #2) | build-verified; the update item hides under one app slot (no release needed to observe) |
| `SENTINEL_APP_INFO` (PR #2) | present at image 0x120 (`SNAP`); release v2.1.3 published. NOT yet observed on a device — and the build currently on the unit predates `version.txt` (bare-hash version), so its Update won't compare until a ≥2.1.3 build is reinstalled |

Hardware findings, not defects in this work:
- **C6 update succeeds, but the activate RPC is a false negative on this factory C6.** The
  user approved flashing the C6 to 2.12.16; GhostESP's opt-in `c6ota force` wrote the image and
  `Req_OTAActivate` **timed out** (the C6 reboots into the new image, so the activate response
  is lost), reported as "activation failed". The image nonetheless **applied after a cold power
  cycle** — the launcher's `info` then read `c6 2.12.16`. So `c6ota force` works on this unit;
  just expect the activate timeout and a power cycle to complete it. (`info unknown` before and
  right after the flash cannot distinguish success from failure — only the post-cold-cycle read
  does.)
- **`panel none` in the launcher after GhostESP runs** (observed). GhostESP does not reset the
  integrated ST712x controller at boot, and after GhostESP ran, Sentinel's `info` read
  `panel none` (was ST7121); a cold power cycle was performed (which also completed the C6
  update). Whether the cold cycle alone restores launcher panel detection was not isolated —
  treat the boot-time ST712x reset above as the robustness fix.

## Residual unknowns / recommendations

- On-hardware validation of the JanOS CP2102N fix (needs a C5 board on this unit's USB-A; none
  was attached). The display/touch and SD paths are now hardware-validated.
- The display warm-reset robustness items above (panel reset pulse, variant-probe retry, touch
  reset, keyboard queue flush) should be done and tested with the panel in hand.
- Release **v2.1.3** is published with both `GhostESP-Tab5-*.bin` assets. For Sentinel's
  in-launcher Update to actually fire, each release must (a) bump `version.txt` so the asset's
  `PROJECT_VER` matches/leads its tag (otherwise Sentinel re-offers the same version forever),
  and (b) be installed on a device built with `version.txt` — the build currently on the test
  unit predates it (bare-hash version), so its Update won't compare until a ≥2.1.3 build is
  reinstalled. End-to-end Update has not yet been observed on a device.
- Not yet observed on a device (build-verified + behaviour-reasoned only): the "Return to
  Launcher" **menu item** on-screen (the `reboot launcher` CLI path IS validated) and the
  update row being hidden under the single slot. A device turn with the user watching would
  confirm these (deferred — the st7121-flicker session holds the unit).
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

## On-device validation & crash fixes (2026-11-19, v2.1.4)

Validated the ST7121 Tab5 (MAC E8:F6:0A:E2:FC:56) under the Sentinel launcher. Beyond the
touch/UX work in PR #11 (reliable ST712x touch, hidden single-slot Update row, home-screen
"Return to Sentinel", top-left Back button), three device-crashing/freezing faults were
root-caused from panic backtraces + a USB-C console capture (ELF `d88336081`) and fixed in
**PR #12**:

- **Scan APs / Scan Stations → tap a row → reboot** (deterministic, `Load access fault`).
  Those result lists render through `options_view` in **virtual mode**, where each row's
  `lv_obj` user_data is an **integer index** (index 0 == `NULL`), not an option string. The
  custom Tab5 input loop (no LVGL `LV_EVENT_CLICKED` dispatch) read that index and passed it to
  `handle_option_directly()` → `option_event_cb()`, which `strcmp()`'d it as a `char*`. Fix:
  `options_view_activate_virtual_row()` routes a virtual-mode tap to the list's activate fn
  (`show_ap_detail`/`show_station_detail`/…); `option_event_cb`/`handle_option_directly`
  back-stop a NULL option. The same loop drives all vlist kinds (AP, STA, scanall, ARP, Govee,
  mDNS, enum, BLE detect/adv/gatt) — all were affected, all now routed.
- **Flock-exit freeze (~15 s)**, not a reboot. `flock_detector_start()` enters monitor mode
  with `flock_sniffer_cb`, unrecognised by `wifi_manager_start_monitor_mode()` → default
  `FILTER_MASK_ALL`; the CTRL-frame flood across a 250 ms / 14-ch hop wedges the SDIO-hosted C6,
  after which teardown RPCs time out ~5 s each. Fix: narrow Flock's promiscuous filter to
  `MGMT|DATA` (what the callback reads). Verified over `flockscan`/`flockstop` CLI: 0 C6
  timeouts in a 12 s scan, teardown 15 s → 0.4 s. Same class as the Airspace/Packet-Monitor
  lockups — **any** `FILTER_MASK_ALL` + channel-hopping monitor wedges the hosted C6. Raw PCAP
  capture and SAE flood intentionally keep `ALL`.
- **Flaky-C6 `sta_get_ap_info` crash.** The vendored (in-tree) esp_hosted
  `rpc_wifi_sta_get_ap_info()` `memcpy`'d from `resp->…out_list` after only a NULL check; a
  flaky slave link returned a SUCCESS response with a non-NULL but garbage pointer (observed
  `0xb816a398`). Now rejected via `esp_ptr_byte_accessible()` before the copy.
