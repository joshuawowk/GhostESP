/*
 * Sentinel app information descriptor.
 *
 * Vendored verbatim from the Sentinel Bootloader SDK
 * (joshuawowk/sentinel-bootloader : sdk/sentinel_app/include/sentinel_app.h).
 * Only this provenance comment was added; the structure layout is a fixed ABI
 * read by the Sentinel launcher straight out of the flashed image, so it must
 * not be changed. See docs/SENTINEL.md.
 *
 * Put this in exactly one source file of your firmware:
 *
 *     #include "sentinel_app.h"
 *     SENTINEL_APP_INFO("https://github.com/<owner>/<repo>", "<release asset>.bin");
 *
 * and keep it in the image: link with -Wl,-u,sentinel_app_info (GhostESP does
 * this from main/CMakeLists.txt, gated on CONFIG_M5STACK_TAB5). Without that the
 * linker drops it, because nothing in the firmware refers to it.
 *
 *   repo   The repository's address, "https://github.com/<owner>/<repo>". Its releases are
 *          where updates come from: Sentinel asks for the newest release that is not a draft
 *          or a pre-release and has the asset below.
 *   asset  The release file to install, as named in a release, such as "my-firmware.bin". A
 *          version in the name is allowed. "" lets Sentinel pick the release's only .bin, or
 *          the only one that names the Tab5 or the ESP32-P4.
 *
 * Updates are offered when the release's tag is a newer version than the image's
 * esp_app_desc_t version (PROJECT_VER).
 *
 * The format is fixed. It sits at SENTINEL_APP_INFO_OFFSET from the start of the image,
 * straight after esp_app_desc_t, in the section ESP-IDF keeps for this
 * (.rodata_custom_desc, "App Image Format" in the ESP-IDF documentation). A firmware that
 * already puts something else there cannot have both.
 */
#pragma once
#include <stdint.h>

#define SENTINEL_APP_INFO_MAGIC     0x50414E53u     /* "SNAP" */
#define SENTINEL_APP_INFO_VERSION   1
/* esp_image_header_t (24) + esp_image_segment_header_t (8) + esp_app_desc_t (256) */
#define SENTINEL_APP_INFO_OFFSET    0x120

typedef struct {
    uint32_t magic;             /* SENTINEL_APP_INFO_MAGIC                                */
    uint16_t version;           /* SENTINEL_APP_INFO_VERSION                              */
    uint16_t size;              /* sizeof(sentinel_app_info_t)                            */
    char repo[120];             /* "https://github.com/<owner>/<repo>", NUL-terminated    */
    char asset[64];             /* release file to update from, or "", NUL-terminated     */
    uint32_t reserved[16];      /* zero                                                   */
} sentinel_app_info_t;

#ifdef __cplusplus
static_assert(sizeof(sentinel_app_info_t) == 256, "sentinel_app_info_t is a fixed format");
#define SENTINEL_APP_INFO_LINKAGE extern "C"
#else
_Static_assert(sizeof(sentinel_app_info_t) == 256, "sentinel_app_info_t is a fixed format");
#define SENTINEL_APP_INFO_LINKAGE
#endif

/* A string too long for its field is a compile error in C++ and a warning in C. */
#define SENTINEL_APP_INFO(repo_url, asset_name)                                                          \
    SENTINEL_APP_INFO_LINKAGE const sentinel_app_info_t sentinel_app_info                                \
        __attribute__((section(".rodata_custom_desc"), used, aligned(4))) = {                            \
            SENTINEL_APP_INFO_MAGIC, SENTINEL_APP_INFO_VERSION, (uint16_t)sizeof(sentinel_app_info_t),   \
            repo_url, asset_name, {0}}
