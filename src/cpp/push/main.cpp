// sm_push — 127.0.0.1:9002 에서 줄 단위 JSON 으로 monitor.db 를 읽어 준다. (push.py 의 C++ 판)
// 단일 스레드 epoll. 프로토콜은 PROTOCOL.md.
//
// 환경변수(.env): SM_DB(monitor.db) SM_PUSH_PORT(9002) SM_RETAIN_DAYS(30)
#include "common/db.h"
#include "common/env.h"
#include "common/log.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sqlite3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <csignal>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

namespace {

constexpr int       kProtoV       = 1;
constexpr size_t    kMaxLine      = 64 * 1024;
constexpr size_t    kMaxOutBuf    = 4 * 1024 * 1024;
constexpr long long kHelloTimeout = 60;
constexpr long long kPingInterval = 30;
constexpr long long kMaxRange     = 30 * 86400;

using Writer = rapidjson::Writer<rapidjson::StringBuffer>;

// ── DB 조회 ──────────────────────────────────────────────────────────────
class Queries {
public:
    explicit Queries(const std::string& path) {
        db_.open(path, SQLITE_OPEN_READONLY);
        channels_ = std::make_unique<sm::Stmt>(db_,
            "SELECT c.channel_id, c.channel_name, c.first_seen_ts, c.last_seen_ts, COUNT(s.ts), MAX(s.viewers), "
            "       (SELECT viewers FROM viewer_samples v WHERE v.channel_id = c.channel_id AND v.ts = c.last_seen_ts) "
            "FROM channels c JOIN viewer_samples s ON s.channel_id = c.channel_id AND s.ts >= ? "
            "GROUP BY c.channel_id ORDER BY MAX(s.viewers) DESC, c.last_seen_ts DESC");
        points_ = std::make_unique<sm::Stmt>(db_,
            "SELECT ts, viewers FROM viewer_samples WHERE channel_id=? AND ts>=? AND ts<=? ORDER BY ts");
        info_ = std::make_unique<sm::Stmt>(db_,
            "SELECT ts, category, title FROM stream_info "
            "WHERE channel_id=?1 AND ts = (SELECT MAX(ts) FROM stream_info WHERE channel_id=?1 AND ts<=?2) "
            "UNION ALL "
            "SELECT ts, category, title FROM stream_info WHERE channel_id=?1 AND ts>?2 AND ts<=?3 "
            "ORDER BY ts");
        stats_ = std::make_unique<sm::Stmt>(db_,
            "SELECT COUNT(*), MIN(ts), MAX(ts), (SELECT COUNT(*) FROM channels) FROM viewer_samples");
    }

    // 최근 7일 최고 시청자수 내림차순. 7일간 샘플 없는 채널은 제외.
    void channels(Writer& w) {
        const long long since = std::time(nullptr) - 7 * 86400;
        w.Key("channels"); w.StartArray();
        channels_->reset().bind(1, since);
        while (channels_->step()) {
            w.StartObject();
            w.Key("channel_id");   w.String(channels_->col_str(0).c_str());
            w.Key("channel_name"); w.String(channels_->col_str(1).c_str());
            w.Key("first_seen");   w.Int64(channels_->col_i64(2));
            w.Key("last_seen");    w.Int64(channels_->col_i64(3));
            w.Key("samples");      w.Int64(channels_->col_i64(4));
            w.Key("peak");         w.Int64(channels_->col_i64(5));
            w.Key("current");      w.Int64(channels_->col_null(6) ? 0 : channels_->col_i64(6));   // 마지막 관측 시청자수
            w.EndObject();
        }
        channels_->reset();
        w.EndArray();
    }

    void samples(Writer& w, const std::string& cid, long long from, long long to) {
        w.Key("channel_id"); w.String(cid.c_str());
        w.Key("from"); w.Int64(from);
        w.Key("to");   w.Int64(to);
        w.Key("points"); w.StartArray();
        points_->reset().bind(1, cid).bind(2, from).bind(3, to);
        while (points_->step()) {
            w.StartArray(); w.Int64(points_->col_i64(0)); w.Int64(points_->col_i64(1)); w.EndArray();
        }
        points_->reset();
        w.EndArray();
        w.Key("info"); w.StartArray();
        info_->reset().bind(1, cid).bind(2, from).bind(3, to);
        while (info_->step()) {
            w.StartArray();
            w.Int64(info_->col_i64(0));
            if (info_->col_null(1)) w.Null(); else w.String(info_->col_str(1).c_str());
            if (info_->col_null(2)) w.Null(); else w.String(info_->col_str(2).c_str());
            w.EndArray();
        }
        info_->reset();
        w.EndArray();
    }

    void stats(Writer& w) {
        stats_->reset();
        if (stats_->step()) {
            w.Key("samples");  w.Int64(stats_->col_i64(0));
            w.Key("oldest");   if (stats_->col_null(1)) w.Null(); else w.Int64(stats_->col_i64(1));
            w.Key("newest");   if (stats_->col_null(2)) w.Null(); else w.Int64(stats_->col_i64(2));
            w.Key("channels"); w.Int64(stats_->col_i64(3));
        }
        stats_->reset();
    }

private:
    sm::Db db_;
    std::unique_ptr<sm::Stmt> channels_, points_, info_, stats_;
};

// ── 연결 ─────────────────────────────────────────────────────────────────
struct Conn {
    int         fd = -1;
    std::string peer;
    std::string in, out;
    long long   opened_at = 0;
    long long   last_ping = 0;
    bool        hello = false;
};

class Server {
public:
    Server(Queries& q, int port, long retain_days) : q_(q), retain_days_(retain_days) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) throw std::runtime_error("socket");
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(port));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0)
            throw std::runtime_error(std::string("bind 127.0.0.1:") + std::to_string(port) + ": " + std::strerror(errno));
        if (::listen(listen_fd_, 64) < 0) throw std::runtime_error("listen");

        ep_ = ::epoll_create1(EPOLL_CLOEXEC);
        add(listen_fd_, EPOLLIN);

        // 1초 타이머: ping / hello 타임아웃
        timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        itimerspec its{}; its.it_interval.tv_sec = 1; its.it_value.tv_sec = 1;
        ::timerfd_settime(timer_fd_, 0, &its, nullptr);
        add(timer_fd_, EPOLLIN);

        // SIGTERM/SIGINT → signalfd
        sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGINT);
        ::sigprocmask(SIG_BLOCK, &mask, nullptr);
        sig_fd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        add(sig_fd_, EPOLLIN);

        sm::info("listening 127.0.0.1:" + std::to_string(port));
    }

    void run() {
        epoll_event evs[64];
        while (!stop_) {
            const int n = ::epoll_wait(ep_, evs, 64, -1);
            if (n < 0) { if (errno == EINTR) continue; throw std::runtime_error("epoll_wait"); }
            for (int i = 0; i < n; ++i) {
                const int fd = evs[i].data.fd;
                if (fd == listen_fd_)      accept_all();
                else if (fd == timer_fd_)  on_tick();
                else if (fd == sig_fd_)    on_signal();
                else                       on_conn(fd, evs[i].events);
            }
        }
        // bye → 남은 것 보내고 종료
        for (auto& [fd, c] : conns_) { send_json(*c, [](Writer& w) { w.Key("type"); w.String("bye"); }); flush(*c); }
        sm::info("stopped");
    }

private:
    void add(int fd, uint32_t ev) { epoll_event e{}; e.events = ev; e.data.fd = fd; ::epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &e); }
    void mod(int fd, uint32_t ev) { epoll_event e{}; e.events = ev; e.data.fd = fd; ::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &e); }

    void accept_all() {
        for (;;) {
            sockaddr_in a{}; socklen_t al = sizeof a;
            const int fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&a), &al, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) break;
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            auto c = std::make_unique<Conn>();
            c->fd = fd;
            c->peer = std::string(inet_ntoa(a.sin_addr)) + ":" + std::to_string(ntohs(a.sin_port));
            c->opened_at = c->last_ping = std::time(nullptr);
            add(fd, EPOLLIN);
            sm::info("conn " + c->peer + " (총 " + std::to_string(conns_.size() + 1) + ")");
            conns_[fd] = std::move(c);
        }
    }

    void close_conn(int fd, const char* why) {
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;
        ::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        sm::info("close " + it->second->peer + " " + why + " (총 " + std::to_string(conns_.size() - 1) + ")");
        conns_.erase(it);
    }

    void on_signal() {
        signalfd_siginfo si;
        while (::read(sig_fd_, &si, sizeof si) == sizeof si) {}
        sm::info("signal received, stopping");
        stop_ = true;
    }

    void on_tick() {
        uint64_t exp;
        while (::read(timer_fd_, &exp, sizeof exp) == sizeof exp) {}
        const long long now = std::time(nullptr);
        std::vector<int> drop;
        for (auto& [fd, c] : conns_) {
            if (!c->hello && now - c->opened_at > kHelloTimeout) { drop.push_back(fd); continue; }
            if (now - c->last_ping >= kPingInterval) {
                send_json(*c, [](Writer& w) { w.Key("type"); w.String("ping"); });
                c->last_ping = now;
                if (!flush(*c)) drop.push_back(fd);
            }
        }
        for (int fd : drop) close_conn(fd, "timeout");
    }

    void on_conn(int fd, uint32_t events) {
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;
        Conn& c = *it->second;
        if (events & (EPOLLERR | EPOLLHUP)) { close_conn(fd, "hup"); return; }
        if (events & EPOLLIN) {
            char buf[16384];
            for (;;) {
                const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
                if (n > 0) {
                    c.in.append(buf, static_cast<size_t>(n));
                    if (c.in.size() > kMaxLine) { close_conn(fd, "line too long"); return; }
                    continue;
                }
                if (n == 0) { close_conn(fd, "peer closed"); return; }
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                close_conn(fd, "recv error"); return;
            }
            for (size_t p; (p = c.in.find('\n')) != std::string::npos;) {
                std::string line = c.in.substr(0, p);
                c.in.erase(0, p + 1);
                if (!line.empty()) handle_line(c, line);
            }
            if (!flush(c)) { close_conn(fd, "send error"); return; }
        }
        if (events & EPOLLOUT) {
            if (!flush(c)) { close_conn(fd, "send error"); return; }
        }
    }

    // 요청 한 줄 처리 → 응답을 c.out 에 쌓는다
    void handle_line(Conn& c, const std::string& line) {
        rapidjson::Document doc;
        if (doc.Parse(line.c_str()).HasParseError() || !doc.IsObject()) {
            send_error(c, nullptr, "bad_json", "JSON 객체가 아님");
            return;
        }
        const std::string type = doc.HasMember("type") && doc["type"].IsString() ? doc["type"].GetString() : "";
        const char* id = doc.HasMember("id") && doc["id"].IsString() ? doc["id"].GetString() : nullptr;

        if (type == "hello") {
            c.hello = true;
            send_json(c, [&](Writer& w) {
                w.Key("type"); w.String("hello"); w.Key("v"); w.Int(kProtoV);
                w.Key("now"); w.Int64(std::time(nullptr)); w.Key("retain_days"); w.Int64(retain_days_);
            }, id);
        } else if (type == "channels") {
            send_json(c, [&](Writer& w) { w.Key("type"); w.String("channels"); q_.channels(w); }, id);
        } else if (type == "samples") {
            const auto cid = doc.FindMember("channel_id");
            if (cid == doc.MemberEnd() || !cid->value.IsString() || cid->value.GetStringLength() == 0) {
                send_error(c, id, "bad_arg", "channel_id 필요"); return;
            }
            const long long now = std::time(nullptr);
            long long to = now, from = 0;
            if (const auto it = doc.FindMember("to"); it != doc.MemberEnd() && it->value.IsInt64() && it->value.GetInt64() > 0)
                to = it->value.GetInt64();
            from = to - 86400;
            if (const auto it = doc.FindMember("from"); it != doc.MemberEnd() && it->value.IsInt64() && it->value.GetInt64() > 0)
                from = it->value.GetInt64();
            if (from > to || to - from > kMaxRange) { send_error(c, id, "bad_arg", "구간은 0~30일"); return; }
            const std::string ch = cid->value.GetString();
            send_json(c, [&](Writer& w) { w.Key("type"); w.String("samples"); q_.samples(w, ch, from, to); }, id);
        } else if (type == "stats") {
            send_json(c, [&](Writer& w) {
                w.Key("type"); w.String("stats"); w.Key("clients"); w.Int64(static_cast<long long>(conns_.size()));
                q_.stats(w);
            }, id);
        } else {
            send_error(c, id, "unknown_type", "type=" + type);
        }
    }

    template <class F>
    void send_json(Conn& c, F&& body, const char* id = nullptr) {
        rapidjson::StringBuffer sb;
        Writer w(sb);
        w.StartObject();
        body(w);
        if (id) { w.Key("id"); w.String(id); }
        w.EndObject();
        c.out.append(sb.GetString(), sb.GetSize());
        c.out.push_back('\n');
    }
    void send_error(Conn& c, const char* id, const char* code, const std::string& msg) {
        send_json(c, [&](Writer& w) {
            w.Key("type"); w.String("error"); w.Key("code"); w.String(code); w.Key("message"); w.String(msg.c_str());
        }, id);
    }

    // c.out 을 보낼 수 있는 만큼 보낸다. 남으면 EPOLLOUT 을 켠다. false = 연결 끊어야 함.
    bool flush(Conn& c) {
        while (!c.out.empty()) {
            const ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
            if (n > 0) { c.out.erase(0, static_cast<size_t>(n)); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (c.out.size() > kMaxOutBuf) return false;
                mod(c.fd, EPOLLIN | EPOLLOUT);
                return true;
            }
            return false;
        }
        mod(c.fd, EPOLLIN);
        return true;
    }

    Queries& q_;
    long retain_days_;
    int listen_fd_ = -1, ep_ = -1, timer_fd_ = -1, sig_fd_ = -1;
    bool stop_ = false;
    std::map<int, std::unique_ptr<Conn>> conns_;
};

} // namespace

int main() {
    sm::load_env();
    const std::string db_path = sm::env_str("SM_DB", "monitor.db");
    const int  port        = static_cast<int>(sm::env_long("SM_PUSH_PORT", 9002));
    const long retain_days = sm::env_long("SM_RETAIN_DAYS", 30);
    try {
        Queries q(db_path);
        Server srv(q, port, retain_days);
        srv.run();
    } catch (const std::exception& e) {
        sm::error(std::string("fatal: ") + e.what());
        return 1;
    }
    return 0;
}
