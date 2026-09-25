#include "flash_log.h"

#if VOLTRA_DIAG

#include <Arduino.h>
#include <LittleFS.h>
#include <stdarg.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" int log_printfv(const char *format, va_list arg);   // esp32-hal-uart.c

namespace {
constexpr const char *LOG_PATH = "/log.txt";
constexpr const char *OLD_PATH = "/log.old";      // previous log.txt once it filled up
constexpr size_t MAX_FILE_BYTES = 1400 * 1024;    // two of these fit the 3.4 MB partition
constexpr size_t BUF_BYTES = 64 * 1024;           // RAM staging between flash writes
constexpr uint32_t FLUSH_MS = 500;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
char *s_buf = nullptr;      // filled by log_printf, drained by the writer task
char *s_spare = nullptr;    // swapped with s_buf for each flush
size_t s_len = 0;
uint32_t s_dropped = 0;     // bytes lost because the staging buffer was full
volatile bool s_quiet = false;   // dumping: keep other log lines off the serial port
File s_file;

void append(const char *text, size_t n)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    if (s_buf) {
        if (s_len + n <= BUF_BYTES) {
            memcpy(s_buf + s_len, text, n);
            s_len += n;
        } else {
            s_dropped += n;
        }
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
}

void open_log()
{
    s_file = LittleFS.open(LOG_PATH, FILE_APPEND, true);
    if (!s_file) log_printf("flash log: cannot open %s\n", LOG_PATH);
}

void flush_to_flash()
{
    portENTER_CRITICAL(&s_mux);
    char *full = s_buf;
    size_t n = s_len;
    uint32_t dropped = s_dropped;
    s_buf = s_spare;
    s_spare = full;
    s_len = 0;
    s_dropped = 0;
    portEXIT_CRITICAL(&s_mux);

    if (!s_file) return;
    if (dropped) s_file.printf("[flash log: %u bytes dropped]\n", (unsigned)dropped);
    if (n) s_file.write(reinterpret_cast<const uint8_t *>(full), n);
    s_file.flush();

    if (s_file.size() > MAX_FILE_BYTES) {
        s_file.close();
        LittleFS.remove(OLD_PATH);
        LittleFS.rename(LOG_PATH, OLD_PATH);
        open_log();
    }
}

void dump_file(const char *path, size_t &total)
{
    File f = LittleFS.open(path, FILE_READ);
    if (!f) return;
    static uint8_t chunk[1024];
    size_t n;
    while ((n = f.read(chunk, sizeof(chunk))) > 0) {
        Serial.write(chunk, n);
        total += n;
    }
    f.close();
}

void handle_command(const String &cmd)
{
    if (cmd == "dump") {
        s_quiet = true;
        flush_to_flash();
        s_file.close();
        size_t total = 0;
        Serial.print("\n=== VOLTRA LOG BEGIN ===\n");
        dump_file(OLD_PATH, total);
        dump_file(LOG_PATH, total);
        Serial.printf("\n=== VOLTRA LOG END %u ===\n", (unsigned)total);
        Serial.flush();
        open_log();
        s_quiet = false;
    } else if (cmd == "clear") {
        s_file.close();
        LittleFS.remove(OLD_PATH);
        LittleFS.remove(LOG_PATH);
        open_log();
        Serial.print("\n=== VOLTRA LOG CLEARED ===\n");
    }
}

void writer_task(void *)
{
    String line;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FLUSH_MS));
        flush_to_flash();
        while (Serial.available()) {
            const char ch = (char)Serial.read();
            if (ch == '\n' || ch == '\r') {
                line.trim();
                if (line.length()) handle_command(line);
                line = "";
            } else if (line.length() < 32) {
                line += ch;
            }
        }
    }
}
}  // namespace

/** Linked in place of log_printf (-Wl,--wrap=log_printf): print as usual, and keep a copy. */
extern "C" int __wrap_log_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int len = 0;
    if (!s_quiet) {
        va_list copy;
        va_copy(copy, args);
        len = log_printfv(format, copy);
        va_end(copy);
    }
    if (s_buf && !xPortInIsrContext()) {
        char local[256];
        va_list copy;
        va_copy(copy, args);
        const int n = vsnprintf(local, sizeof(local), format, copy);
        va_end(copy);
        if (n >= (int)sizeof(local)) {
            if (char *big = static_cast<char *>(malloc(n + 1))) {
                vsnprintf(big, n + 1, format, args);
                append(big, n);
                free(big);
            }
        } else if (n > 0) {
            append(local, n);
        }
        if (len == 0) len = n;
    }
    va_end(args);
    return len;
}

void flash_log_begin()
{
    if (!LittleFS.begin(true)) {
        log_e("flash log: LittleFS mount failed");
        return;
    }
    s_spare = static_cast<char *>(heap_caps_malloc(BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    char *buf = static_cast<char *>(heap_caps_malloc(BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_spare || !buf) {
        log_e("flash log: no memory");
        return;
    }
    open_log();
    if (s_file) s_file.printf("\n===== boot (log %u bytes, fs %u/%u used) =====\n", (unsigned)s_file.size(),
                              (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    portENTER_CRITICAL(&s_mux);
    s_buf = buf;
    portEXIT_CRITICAL(&s_mux);
    xTaskCreatePinnedToCore(writer_task, "flashlog", 6144, nullptr, 1, nullptr, 0);
    log_i("flash log: saving to %s", LOG_PATH);
}

#endif
