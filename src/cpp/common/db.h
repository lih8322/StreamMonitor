#pragma once
// sqlite3 얇은 래퍼. 예외로 오류를 알리고, Stmt 는 RAII 로 finalize 한다.
#include <sqlite3.h>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace sm {

class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Db {
public:
    Db() = default;
    ~Db() { close(); }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    // flags: SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE (쓰기) / SQLITE_OPEN_READONLY (읽기)
    void open(const std::string& path, int flags) {
        if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
            const std::string msg = db_ ? sqlite3_errmsg(db_) : "open failed";
            close();
            throw DbError("sqlite open " + path + ": " + msg);
        }
        sqlite3_busy_timeout(db_, 30000);
    }
    void close() {
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
    }
    sqlite3* raw() const { return db_; }

    void exec(const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "?";
            sqlite3_free(err);
            throw DbError("sqlite exec: " + msg + " -- " + sql.substr(0, 80));
        }
    }
    int changes() const { return sqlite3_changes(db_); }

private:
    sqlite3* db_ = nullptr;
};

class Stmt {
public:
    Stmt(Db& db, const char* sql) {
        if (sqlite3_prepare_v2(db.raw(), sql, -1, &st_, nullptr) != SQLITE_OK)
            throw DbError(std::string("sqlite prepare: ") + sqlite3_errmsg(db.raw()) + " -- " + sql);
    }
    ~Stmt() { if (st_) sqlite3_finalize(st_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& reset() { sqlite3_reset(st_); sqlite3_clear_bindings(st_); return *this; }
    Stmt& bind(int i, std::int64_t v) { sqlite3_bind_int64(st_, i, v); return *this; }
    Stmt& bind(int i, const std::string& v) {
        sqlite3_bind_text(st_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT); return *this;
    }
    Stmt& bind_null(int i) { sqlite3_bind_null(st_, i); return *this; }

    // true = 행 있음(SQLITE_ROW), false = 끝(SQLITE_DONE)
    bool step() {
        const int rc = sqlite3_step(st_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw DbError(std::string("sqlite step: ") + sqlite3_errmsg(sqlite3_db_handle(st_)));
    }
    void run() { while (step()) {} }   // 결과를 버리는 실행

    std::int64_t col_i64(int i) const { return sqlite3_column_int64(st_, i); }
    bool col_null(int i) const { return sqlite3_column_type(st_, i) == SQLITE_NULL; }
    std::string col_str(int i) const {
        const unsigned char* p = sqlite3_column_text(st_, i);
        return p ? reinterpret_cast<const char*>(p) : std::string{};
    }

private:
    sqlite3_stmt* st_ = nullptr;
};

// 스키마 — src/collector/chzzk/schema.sql 과 같은 내용. 바이너리 하나로 돌도록 내장한다.
inline const char* kSchemaSql = R"SQL(
PRAGMA journal_mode = WAL;
CREATE TABLE IF NOT EXISTS viewer_samples (
    channel_id TEXT    NOT NULL,
    ts         INTEGER NOT NULL,
    viewers    INTEGER NOT NULL,
    PRIMARY KEY (channel_id, ts)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_samples_ts ON viewer_samples(ts);
CREATE TABLE IF NOT EXISTS stream_info (
    channel_id TEXT    NOT NULL,
    ts         INTEGER NOT NULL,
    category   TEXT,
    title      TEXT,
    PRIMARY KEY (channel_id, ts)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS channels (
    channel_id    TEXT PRIMARY KEY,
    channel_name  TEXT NOT NULL,
    first_seen_ts INTEGER NOT NULL,
    last_seen_ts  INTEGER NOT NULL
);
)SQL";

} // namespace sm
