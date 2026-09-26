#pragma once
/**
 * Diagnostic builds only (VOLTRA_DIAG=1): keeps a copy of the serial log in flash, so a
 * capture can be taken at the Voltra with no computer attached and pulled off later:
 *
 *   ~/.platformio/penv/bin/python tools/pull_log.py
 *
 * Every log_* line is appended to /log.txt on the "spiffs" partition (LittleFS), with a
 * marker at each boot. Over USB serial, "dump" prints the saved log between markers and
 * "clear" deletes it. Needs -Wl,--wrap=log_printf. A no-op in release builds.
 */
#ifndef VOLTRA_DIAG
#define VOLTRA_DIAG 0
#endif

#if VOLTRA_DIAG
void flash_log_begin();
#else
inline void flash_log_begin() {}
#endif
