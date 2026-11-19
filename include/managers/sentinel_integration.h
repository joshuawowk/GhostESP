/*
 * Sentinel launcher integration helpers.
 *
 * GhostESP installed through the Sentinel Bootloader (joshuawowk/sentinel-bootloader)
 * runs from a per-firmware partition view: its image is `ota_0`, there is no
 * `factory` partition, and a blank `otadata` selects the launcher. These helpers
 * detect that situation and provide the documented way back to the launcher
 * (erase `otadata`, restart). They are universal: on a directly-flashed GhostESP
 * (running from `factory`) the detectors return false, so the "Return to launcher"
 * UI and CLI stay hidden/inert. See docs/SENTINEL.md.
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when GhostESP was chain-loaded by a launcher, i.e. it is running from an
 * OTA app slot rather than from a `factory` partition (a direct cable flash). */
bool sentinel_launcher_boot_detected(void);

/* True specifically under the Sentinel Bootloader: a launcher boot AND no
 * `factory` app partition in the running view (the Sentinel signature, per
 * sentinel-bootloader/docs/firmware-authors.md). Under bmorcelli/Launcher a
 * factory partition (the launcher itself) is present, so this is false. */
bool sentinel_boot_detected(void);

/* Return control to the launcher: erase `otadata` and restart. A blank `otadata`
 * selects the launcher under Sentinel, and the factory app (the launcher) under
 * bmorcelli/Launcher, so this works under both. Does not return. */
void sentinel_return_to_launcher(void);

#ifdef __cplusplus
}
#endif
