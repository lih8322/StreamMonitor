#pragma once
// `.env` 를 읽어 환경변수에 넣는다 (이미 있는 키는 유지) — Python 판의 load_env 와 같은 규칙.
#include <cstdlib>
#include <fstream>
#include <string>

namespace sm {

inline void load_env(const std::string& path = ".env") {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        // trim
        const auto b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos) continue;
        const auto e = line.find_last_not_of(" \t\r");
        line = line.substr(b, e - b + 1);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        const auto ke = k.find_last_not_of(" \t"); k = k.substr(0, ke + 1);
        const auto vb = v.find_first_not_of(" \t"); v = vb == std::string::npos ? "" : v.substr(vb);
        ::setenv(k.c_str(), v.c_str(), /*overwrite=*/0);
    }
}

inline std::string env_str(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : def;
}

inline long env_long(const char* key, long def) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::atol(v) : def;
}

} // namespace sm
