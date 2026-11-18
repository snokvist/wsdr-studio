#include "log.h"

#include <atomic>
#include <cstdio>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace kp {
namespace {
std::atomic<LogSink> g_sink{nullptr};
std::atomic<bool> g_debug{false};
}  // namespace

void set_log_sink(LogSink s) { g_sink = s; }
void set_log_debug(bool on) { g_debug = on; }

void logf(LogLevel l, const char *fmt, ...) {
  if (l == LogLevel::Debug && !g_debug) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
#ifdef __ANDROID__
  static const int prio[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
  __android_log_write(prio[static_cast<int>(l)], "kepyur", buf);
#else
  static const char *tag[] = {"D", "I", "W", "E"};
  std::fprintf(stderr, "[kepyur %s] %s\n", tag[static_cast<int>(l)], buf);
#endif
  if (LogSink s = g_sink) s(l, buf);
}
}  // namespace kp
