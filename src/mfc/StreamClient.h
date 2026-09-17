#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// oracle 의 streammonitor-push 와 통신한다.
//
// 서버는 127.0.0.1:9002 에만 바인딩돼 있어 SSH 터널로만 닿는다. 터널은 이 클래스가
// 직접 띄운다 (Prism FlowClient 와 같은 방식):
//     ssh -N -L 9002:127.0.0.1:9002 oracle
// 프로토콜은 줄 단위 JSON — PROTOCOL.md.

namespace sm {

struct Channel {
    std::wstring id;
    std::wstring name;
    long long    first_seen = 0;
    long long    last_seen  = 0;
    int          samples    = 0;
    int          peak       = 0;   // 최근 7일 최고 시청자수
};

struct Point { long long ts = 0; int viewers = 0; };
struct Info  { long long ts = 0; std::wstring category; std::wstring title; };

struct Samples {
    std::wstring       channel_id;
    long long          from = 0, to = 0;
    std::vector<Point> points;
    std::vector<Info>  info;
};

class StreamClient {
public:
    StreamClient() = default;
    ~StreamClient();
    StreamClient(const StreamClient&)            = delete;
    StreamClient& operator=(const StreamClient&) = delete;

    // 수신 스레드에서 호출된다 — UI 는 PostMessage 로.
    using ChannelsFn = std::function<void(std::unique_ptr<std::vector<Channel>>)>;
    using SamplesFn  = std::function<void(std::unique_ptr<Samples>)>;
    using StatusFn   = std::function<void(const std::wstring&)>;

    void set_handlers(ChannelsFn on_channels, SamplesFn on_samples, StatusFn on_status);

    void start();
    void stop();

    // 요청. 연결 전이면 false.
    bool request_channels();
    bool request_samples(const std::wstring& channel_id, long long from, long long to);

    [[nodiscard]] bool connected() const { return connected_.load(); }

private:
    void run();
    bool ensure_tunnel();
    void close_tunnel();
    bool connect_once();
    void close_socket();
    bool send_line(const std::string& utf8_line);
    void handle_line(const std::string& utf8_line);
    void status(const std::wstring& msg);

    static constexpr const char* kHost = "127.0.0.1";
    static constexpr int         kPort = 9002;
    static constexpr int         kReconnectMs = 5000;

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    std::mutex         sock_mutex_;
    unsigned long long sock_ = ~0ull;

    void* tunnel_process_ = nullptr;
    void* tunnel_job_     = nullptr;

    std::atomic<int> next_id_{1};

    std::mutex handler_mutex_;
    ChannelsFn on_channels_;
    SamplesFn  on_samples_;
    StatusFn   on_status_;
};

} // namespace sm
