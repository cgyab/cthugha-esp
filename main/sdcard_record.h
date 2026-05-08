#pragma once
#include <stdbool.h>
#include <stdint.h>

// SD card AVI recording.  Must be enabled via Kconfig (CTHUGHA_SDCARD_ENABLED).
// When disabled, all calls compile to no-ops.

#ifdef CONFIG_CTHUGHA_SDCARD_ENABLED
void sdcard_record_init(void);
void sdcard_record_toggle(void);
bool sdcard_record_is_active(void);
// Called every render frame.  Internally skips frames to match the target FPS.
void sdcard_record_frame(const uint8_t *indexed_buf);
#else
static inline void sdcard_record_init(void)   {}
static inline void sdcard_record_toggle(void) {}
static inline bool sdcard_record_is_active(void) { return false; }
static inline void sdcard_record_frame(const uint8_t *b) { (void)b; }
#endif
