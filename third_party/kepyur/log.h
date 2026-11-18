// kepyur: tiny logging shim. Android: logcat (tag "kepyur") + the app's log file sink; desktop: stderr.
#pragma once
#include <cstdarg>

namespace kp {
enum class LogLevel { Debug, Info, Warn, Error };
// Optional extra sink (the app's log file). Called with a formatted line, no trailing newline.
using LogSink = void (*)(LogLevel, const char *line);
void set_log_sink(LogSink s);
void set_log_debug(bool on);
void logf(LogLevel l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
}  // namespace kp

#define KP_LOGD(...) ::kp::logf(::kp::LogLevel::Debug, __VA_ARGS__)
#define KP_LOGI(...) ::kp::logf(::kp::LogLevel::Info, __VA_ARGS__)
#define KP_LOGW(...) ::kp::logf(::kp::LogLevel::Warn, __VA_ARGS__)
#define KP_LOGE(...) ::kp::logf(::kp::LogLevel::Error, __VA_ARGS__)
