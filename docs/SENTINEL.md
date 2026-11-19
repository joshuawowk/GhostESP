# GhostESP on the M5Stack Tab5 under the Sentinel Bootloader

This describes how the GhostESP Tab5 port behaves under the
[Sentinel Bootloader](https://github.com/joshuawowk/sentinel-bootloader) launcher, and
what was added to make GhostESP a well-behaved Sentinel firmware. For the general Tab5
port (toolchain, display, C5/JanOS add-on) see [`M5STACK_TAB5.md`](M5STACK_TAB5.md) and
[`M5STACK_TAB5_DEVNOTES.md`](M5STACK_TAB5_DEVNOTES.md).

Sentinel gives every installed firmware its **own partition-table view**: GhostESP's image
is installed as `ota_0`, with its own `nvs`, `phy_init` and `slave_fw` (the bundled C6
image), and **no `factory` partition**. A blank `otadata` selects the launcher. This is the
opposite of the shared-table bmorcelli/Launcher the port originally targeted, and it changes
three things GhostESP had to account for.

## What GhostESP sees under Sentinel

| Area | Under Sentinel | GhostESP behaviour |
|---|---|---|
| App slot | single `ota_0` (running); no `factory`, no second OTA slot | self-update is reported as unsupported and hidden (see below) |
| NVS / data | private `nvs`, `phy_init`, `slave_fw` only | finds `slave_fw` by name; NVS starts empty on first install |
| Radio | onboard C6 over SDIO, shared with every firmware | compares the bundled 2.12.16 C6 image to the running one, flashes only on mismatch |
| Way back | erase `otadata` + restart | new "Return to Launcher" menu item and `reboot launcher` CLI |

GhostESP already **worked** under Sentinel as of Sentinel #13 (which restores the UART0
clock the launcher had gated before starting a firmware — without it GhostESP's UART0
console hung at `uart_param_config` with an interrupt-watchdog panic). The changes here are
about being a good citizen, not about booting.

## Installing GhostESP under Sentinel

Once Sentinel is flashed, **do not** `idf.py flash` or `esptool write-flash 0x0 <image>` —
that overwrites the launcher. Install the merged image through Sentinel instead:

```bash
# from the sentinel-bootloader repo, with the Tab5 on USB-C:
python3 tools/sentinelctl.py -p <tab5-port> install local_builds/GhostESP-Tab5-Landscape.bin GhostESP
# during development, install+run+log in one step:
python3 tools/sentinelctl.py -p <tab5-port> run local_builds/GhostESP-Tab5-Landscape.bin
```

Always pin `-p <tab5-port>` (or `export SENTINEL_PORT=…`) to the intended device. Build and
package the image exactly as before (see [`M5STACK_TAB5.md`](M5STACK_TAB5.md)):

```bash
source ~/.espressif/v6.1/esp-idf/export.sh
idf.py set-target esp32p4 && idf.py build
python3 scripts/package_tab5_launcher.py --build-dir build --out local_builds --name GhostESP-Tab5-Landscape
```

The merged image is identical to what a direct `esptool write-flash 0x0` would write; Sentinel
keeps the app row (as `ota_0`), `nvs`, `phy_init` and `slave_fw` (which carries the C6 image).

## Returning to the launcher

GhostESP had no way back — you had to press reset. Added:

- **Settings → System → Backup & Reset → "Return to Launcher"** (house icon). It is shown
  only when GhostESP was chain-loaded by a launcher (running from an OTA slot, not `factory`),
  so it stays hidden on a directly-flashed GhostESP. The confirm dialog reads "Return to
  Sentinel?" when the Sentinel signature is detected (OTA-slot boot **and** no `factory`
  partition), otherwise "Return to launcher?".
- **CLI: `reboot launcher`** (alias `reboot sentinel`) — the same action for a headless
  console (USB-C serial), so the return path can be driven and verified without the touchscreen.

Both call `sentinel_return_to_launcher()`, which erases `otadata` and restarts. A blank
`otadata` selects the launcher under Sentinel, and the factory app (the launcher) under
bmorcelli/Launcher, so it is correct under both. Implemented in
[`main/managers/sentinel_integration.c`](../main/managers/sentinel_integration.c).

## Self-update under a single slot

With one app slot there is nowhere for an over-the-air update to go:
`esp_ota_get_next_update_partition()` returns the running partition, and `esp_ota_begin()`
would fail with `ESP_ERR_OTA_PARTITION_CONFLICT`. `ota_manager_is_supported()` now also
requires the next partition to differ from the running one, so the on-device update item,
the auto-update check, and the WebUI update all report *unsupported* and hide cleanly instead
of offering an update that always errors. Update GhostESP by installing the new image through
Sentinel (same project name → in-place update keeping NVS and data).

## In-launcher updates (SENTINEL_APP_INFO)

So Sentinel can offer **Update** from GitHub releases however GhostESP was installed, the
Tab5 image carries a fixed `sentinel_app_info` descriptor (`SNAP` magic at image offset
`0x120`, in `.rodata_custom_desc`). It names the repository and release asset:

- repo: `https://github.com/joshuawowk/GhostESP`
- asset: `GhostESP-Tab5-Landscape.bin` / `GhostESP-Tab5-Portrait.bin` (per orientation)

It is defined in `main/managers/sentinel_integration.c` under `CONFIG_M5STACK_TAB5`, kept in
the image by `-u sentinel_app_info` in `main/CMakeLists.txt` (same guard), and vendored from
the Sentinel SDK as [`include/sentinel_app.h`](../include/sentinel_app.h). Sentinel offers an
update when a GitHub **release** on that repository has a tag that is a newer version than the
build's `PROJECT_VER` (set from [`version.txt`](../version.txt)) and carries the named asset.

**Releasing:** bump `version.txt` for every release *before* building, and tag the release to
match (e.g. `version.txt` = `2.1.4`, tag `v2.1.4`). If the asset's `PROJECT_VER` lags its tag,
Sentinel re-offers the same version forever; if it leads, the release is never offered. Release
**v2.1.3** (both Tab5 images) is published. A device must be running a build made with
`version.txt` for its version to compare — a bare-`git describe` hash does not. Verify the
descriptor is in a build with:

```bash
python3 -c "import struct,sys; d=open(sys.argv[1],'rb').read(); \
print(struct.unpack_from('<IHH', d, 0x120))" build/Ghost_ESP_IDF.bin
# -> (1346457171, 1, 256)   # 1346457171 == 0x50414E53 'SNAP'
```

## The ESP32-C6 radio — never auto-replaced under a launcher

Every firmware on the Tab5 shares the one esp-hosted slave firmware on the C6, so replacing it
without asking breaks the others. GhostESP bundles ESP-Hosted-MCU **2.12.16** in `slave_fw`.
Under a launcher (running from an OTA slot) GhostESP **does not** touch the C6 at boot — it
logs `Launcher boot: not auto-updating the shared C6` and leaves it alone; an unknown current
version is also treated as "do nothing". The C6 is updated only on the explicit opt-in
`c6ota force` console command. (On a directly-flashed GhostESP, which owns the whole device,
the boot-time auto-update is kept, and now flashes only on a *confirmed* version mismatch or a
failed query — provisioning a blank C6.)

Observed on the ST7121 test unit (factory C6, version `unknown` to the host): `c6ota force`
wrote the 2.12.16 image, `Req_OTAActivate` **timed out** (the C6 reboots into the new image
and the activate RPC response is lost), and the image **applied after a cold power cycle** —
the launcher's `info` then read `c6 2.12.16`. So on this C6 the activate RPC is a false
negative; the write succeeds and a power cycle completes it.

## Configuration facts verified against Sentinel's traps

Checked in `configs/sdkconfig.m5stack_tab5_landscape` and the generated `sdkconfig`:

- `CONFIG_CACHE_L2_CACHE_LINE_64B=y` with `ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` **off** — avoids
  the intermittent `sdmmc_host_check_buffer_alignment` failure (`ret 0x102` → "Unrecoverable
  host sdio state") that a 128 B line + PSRAM mempool causes on IDF ≥ 5.5.
- `CONFIG_FREERTOS_HZ=1000` — esp-hosted asks for a 1000 Hz tick.
- `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` **not set** — avoids the "reject every device"
  enumeration trap for the USB-A/JanOS path.
- SDIO pins CLK 12 / CMD 13 / D0–D3 11·10·9·8, C6 reset 15 — the Tab5's pins.
