#pragma once
// macOS shim for Veyra's logger: one line per event to stderr and to the log file.
#include <string>
namespace veyra::log {
void write(const char* level, const std::string& area, const std::string& message);
inline void info(const std::string& a, const std::string& m) { write("info", a, m); }
inline void warn(const std::string& a, const std::string& m) { write("warn", a, m); }
inline void error(const std::string& a, const std::string& m) { write("error", a, m); }
}
