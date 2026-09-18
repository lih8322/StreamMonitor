// sm_collector — 매분 CHZZK 라이브 상위 N개를 SQLite 에 기록한다. (monitor.py 의 C++ 판)
//
//   sm_collector            데몬 (systemd)
//   sm_collector --once     1회 실행 후 종료
//
// 환경변수(.env): CHZZK_CLIENT_ID, CHZZK_CLIENT_SECRET,
//   SM_DB(monitor.db) SM_TOP_N(50) SM_RETAIN_DAYS(30) SM_POLL_SEC(60)
#include "common/db.h"
#include "common/env.h"
#include "common/log.h"

#include <curl/curl.h>
#include <rapidjson/document.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kApiHost = "https://openapi.chzzk.naver.com";
constexpr int kPageSize = 20;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Live {
    std::string channel_id, channel_name, category, title;
    long long   viewers = 0;
};

// ── HTTP ─────────────────────────────────────────────────────────────────
class Http {
public:
    Http(const std::string& client_id, const std::string& client_secret) {
        curl_ = curl_easy_init();
        if (!curl_) throw std::runtime_error("curl_easy_init");
        headers_ = curl_slist_append(headers_, ("Client-Id: " + client_id).c_str());
        headers_ = curl_slist_append(headers_, ("Client-Secret: " + client_secret).c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &Http::write_cb);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, "");
    }
    ~Http() {
        curl_slist_free_all(headers_);
        if (curl_) curl_easy_cleanup(curl_);
    }
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;

    // GET. 응답 본문과 HTTP 상태를 돌려준다. 전송 실패는 예외.
    std::pair<long, std::string> get(const std::string& url) {
        std::string body;
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &body);
        const CURLcode rc = curl_easy_perform(curl_);
        if (rc != CURLE_OK) throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(rc));
        long status = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
        return {status, body};
    }

private:
    static size_t write_cb(char* p, size_t sz, size_t n, void* ud) {
        static_cast<std::string*>(ud)->append(p, sz * n);
        return sz * n;
    }
    CURL* curl_ = nullptr;
    curl_slist* headers_ = nullptr;
};

std::string json_str(const rapidjson::Value& v, const char* key) {
    const auto it = v.FindMember(key);
    return (it != v.MemberEnd() && it->value.IsString()) ? it->value.GetString() : std::string{};
}

// /open/v1/lives 를 next 커서로 이어 받아 상위 top_n 개.
std::vector<Live> fetch_top_lives(Http& http, int top_n) {
    std::vector<Live> lives;
    std::string next;
    while (static_cast<int>(lives.size()) < top_n) {
        const int size = std::min(kPageSize, top_n - static_cast<int>(lives.size()));
        std::string url = std::string(kApiHost) + "/open/v1/lives?size=" + std::to_string(size);
        if (!next.empty()) {
            char* esc = curl_easy_escape(nullptr, next.c_str(), 0);
            url += "&next=" + std::string(esc);
            curl_free(esc);
        }
        const auto [status, body] = http.get(url);
        rapidjson::Document doc;
        if (doc.Parse(body.c_str()).HasParseError() || !doc.IsObject())
            throw std::runtime_error("lives API: bad JSON (HTTP " + std::to_string(status) + ")");
        const auto code = doc.FindMember("code");
        if (status != 200 || code == doc.MemberEnd() || !code->value.IsInt() || code->value.GetInt() != 200)
            throw std::runtime_error("lives API HTTP " + std::to_string(status) + ": " + body.substr(0, 200));

        const auto& content = doc["content"];
        const auto data = content.FindMember("data");
        int got = 0;
        if (data != content.MemberEnd() && data->value.IsArray()) {
            for (const auto& lv : data->value.GetArray()) {
                if (!lv.IsObject()) continue;
                Live l;
                l.channel_id   = json_str(lv, "channelId");
                l.channel_name = json_str(lv, "channelName");
                l.category     = json_str(lv, "liveCategoryValue");
                l.title        = json_str(lv, "liveTitle");
                if (const auto it = lv.FindMember("concurrentUserCount"); it != lv.MemberEnd() && it->value.IsInt64())
                    l.viewers = it->value.GetInt64();
                if (!l.channel_id.empty()) { lives.push_back(std::move(l)); ++got; }
            }
        }
        next.clear();
        if (const auto page = content.FindMember("page"); page != content.MemberEnd() && page->value.IsObject())
            next = json_str(page->value, "next");
        if (got == 0 || next.empty()) break;
    }
    if (static_cast<int>(lives.size()) > top_n) lives.resize(top_n);
    return lives;
}

// ── DB 기록 ──────────────────────────────────────────────────────────────
class Store {
public:
    explicit Store(const std::string& path) {
        db_.open(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        db_.exec(sm::kSchemaSql);
        ins_sample_ = std::make_unique<sm::Stmt>(db_,
            "INSERT OR IGNORE INTO viewer_samples (channel_id, ts, viewers) VALUES (?,?,?)");
        upsert_channel_ = std::make_unique<sm::Stmt>(db_,
            "INSERT INTO channels (channel_id, channel_name, first_seen_ts, last_seen_ts) VALUES (?,?,?,?) "
            "ON CONFLICT(channel_id) DO UPDATE SET channel_name = excluded.channel_name, last_seen_ts = excluded.last_seen_ts");
        last_info_ = std::make_unique<sm::Stmt>(db_,
            "SELECT category, title FROM stream_info WHERE channel_id=? ORDER BY ts DESC LIMIT 1");
        ins_info_ = std::make_unique<sm::Stmt>(db_,
            "INSERT OR IGNORE INTO stream_info (channel_id, ts, category, title) VALUES (?,?,?,?)");
    }

    // 한 번의 폴링 결과를 3개 테이블에 기록. 제목/카테고리가 바뀐 채널 수 반환.
    int record(long long ts, const std::vector<Live>& lives) {
        int changed = 0;
        db_.exec("BEGIN");
        try {
            for (const auto& l : lives) {
                ins_sample_->reset().bind(1, l.channel_id).bind(2, ts).bind(3, l.viewers).run();
                upsert_channel_->reset().bind(1, l.channel_id).bind(2, l.channel_name).bind(3, ts).bind(4, ts).run();

                last_info_->reset().bind(1, l.channel_id);
                bool same = false;
                if (last_info_->step())
                    same = last_info_->col_str(0) == l.category && last_info_->col_str(1) == l.title;
                last_info_->reset();
                if (!same) {
                    ins_info_->reset().bind(1, l.channel_id).bind(2, ts).bind(3, l.category).bind(4, l.title).run();
                    ++changed;
                }
            }
            db_.exec("COMMIT");
        } catch (...) {
            db_.exec("ROLLBACK");
            throw;
        }
        return changed;
    }

    void purge(long long now, long retain_days) {
        const long long cutoff = now - retain_days * 86400LL;
        db_.exec("DELETE FROM viewer_samples WHERE ts < " + std::to_string(cutoff));
        const int n = db_.changes();
        db_.exec("DELETE FROM stream_info WHERE ts < " + std::to_string(cutoff) +
                 " AND ts < (SELECT MAX(ts) FROM stream_info i WHERE i.channel_id = stream_info.channel_id)");
        const int m = db_.changes();
        if (n || m) sm::info("purged samples=" + std::to_string(n) + " stream_info=" + std::to_string(m));
    }

private:
    sm::Db db_;
    std::unique_ptr<sm::Stmt> ins_sample_, upsert_channel_, last_info_, ins_info_;
};

void run_once(Http& http, Store& store, int top_n, long poll_sec) {
    const long long ts = std::time(nullptr) / poll_sec * poll_sec;
    const auto lives = fetch_top_lives(http, top_n);
    const int changed = store.record(ts, lives);
    if (changed) sm::info("stream_info changed: " + std::to_string(changed));
    std::string top = lives.empty() ? "-" : lives[0].channel_name + "(" + std::to_string(lives[0].viewers) + ")";
    sm::info("ts=" + std::to_string(ts) + " recorded=" + std::to_string(lives.size()) + " top=" + top);
}

} // namespace

int main(int argc, char** argv) {
    sm::load_env();
    const std::string client_id     = sm::env_str("CHZZK_CLIENT_ID", "");
    const std::string client_secret = sm::env_str("CHZZK_CLIENT_SECRET", "");
    if (client_id.empty() || client_secret.empty()) {
        sm::error("CHZZK_CLIENT_ID / CHZZK_CLIENT_SECRET 필요");
        return 1;
    }
    const std::string db_path = sm::env_str("SM_DB", "monitor.db");
    const int  top_n       = static_cast<int>(sm::env_long("SM_TOP_N", 50));
    const long retain_days = sm::env_long("SM_RETAIN_DAYS", 30);
    const long poll_sec    = std::max(1L, sm::env_long("SM_POLL_SEC", 60));
    const bool once = argc > 1 && std::strcmp(argv[1], "--once") == 0;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    try {
        Http  http(client_id, client_secret);
        Store store(db_path);

        if (once) {
            run_once(http, store, top_n, poll_sec);
            curl_global_cleanup();
            return 0;
        }

        std::signal(SIGTERM, on_signal);
        std::signal(SIGINT, on_signal);
        sm::info("started: poll=" + std::to_string(poll_sec) + "s top_n=" + std::to_string(top_n) +
                 " retain_days=" + std::to_string(retain_days) + " db=" + db_path);

        std::time_t last_purge = 0;
        while (!g_stop) {
            try {
                run_once(http, store, top_n, poll_sec);
                if (std::time(nullptr) - last_purge > 3600) {
                    store.purge(std::time(nullptr), retain_days);
                    last_purge = std::time(nullptr);
                }
            } catch (const std::exception& e) {
                sm::error(std::string("poll failed: ") + e.what());
            }
            // 다음 폴링 경계까지 대기 (1초 단위로 stop 확인)
            const std::time_t wake = (std::time(nullptr) / poll_sec + 1) * poll_sec;
            while (!g_stop && std::time(nullptr) < wake)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        sm::info("stopped");
    } catch (const std::exception& e) {
        sm::error(std::string("fatal: ") + e.what());
        curl_global_cleanup();
        return 1;
    }
    curl_global_cleanup();
    return 0;
}
