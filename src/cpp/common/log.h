#pragma once
// stdout 로그 (journald 가 시각을 붙이지만 Python 판과 같은 형식으로 남긴다).
#include <cstdio>
#include <ctime>
#include <string>

namespace sm {

inline void log(const char* level, const std::string& msg) {
    char ts[32];
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    std::fprintf(stdout, "%s %s %s\n", ts, level, msg.c_str());
    std::fflush(stdout);
}
inline void info(const std::string& m)  { log("INFO", m); }
inline void error(const std::string& m) { log("ERROR", m); }

} // namespace sm
