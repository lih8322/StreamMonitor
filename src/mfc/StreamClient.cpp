#include "pch.h"
#include "StreamClient.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace sm {

namespace {

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring json_wstr(const rapidjson::Value& v, const char* key) {
    const auto it = v.FindMember(key);
    return (it != v.MemberEnd() && it->value.IsString()) ? utf8_to_wide(it->value.GetString()) : std::wstring{};
}

long long json_i64(const rapidjson::Value& v, const char* key) {
    const auto it = v.FindMember(key);
    return (it != v.MemberEnd() && it->value.IsInt64()) ? it->value.GetInt64() : 0;
}

// 32비트 빌드 시 WOW64 리다이렉션 때문에 Sysnative 를 먼저 본다 (Prism 과 동일).
const std::wstring& ssh_path() {
    static const std::wstring path = [] {
        wchar_t win[MAX_PATH]{};
        if (::GetWindowsDirectoryW(win, MAX_PATH) == 0) return std::wstring(L"ssh.exe");
        for (const wchar_t* sub : {L"\\Sysnative\\OpenSSH\\ssh.exe", L"\\System32\\OpenSSH\\ssh.exe"}) {
            std::wstring cand = std::wstring(win) + sub;
            if (::GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) return cand;
        }
        return std::wstring(L"ssh.exe");
    }();
    return path;
}

struct WinsockInit {
    WinsockInit() { WSADATA d{}; ::WSAStartup(MAKEWORD(2, 2), &d); }
    ~WinsockInit() { ::WSACleanup(); }
};
WinsockInit g_winsock;

} // namespace

StreamClient::~StreamClient() { stop(); }

void StreamClient::set_handlers(ChannelsFn on_channels, SamplesFn on_samples, StatusFn on_status) {
    std::lock_guard<std::mutex> lk(handler_mutex_);
    on_channels_ = std::move(on_channels);
    on_samples_  = std::move(on_samples);
    on_status_   = std::move(on_status);
}

void StreamClient::status(const std::wstring& msg) {
    StatusFn fn;
    { std::lock_guard<std::mutex> lk(handler_mutex_); fn = on_status_; }
    if (fn) fn(msg);
}

void StreamClient::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&StreamClient::run, this);
}

void StreamClient::stop() {
    if (!running_.exchange(false)) return;
    close_socket();
    if (thread_.joinable()) thread_.join();
    close_tunnel();
}

bool StreamClient::request_channels() {
    if (!connected_.load()) return false;
    return send_line("{\"type\":\"channels\",\"id\":\"c" + std::to_string(next_id_++) + "\"}");
}

bool StreamClient::request_samples(const std::wstring& channel_id, long long from, long long to) {
    if (!connected_.load()) return false;
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("type"); w.String("samples");
    w.Key("id");   w.String(("q" + std::to_string(next_id_++)).c_str());
    w.Key("channel_id"); w.String(wide_to_utf8(channel_id).c_str());
    w.Key("from"); w.Int64(from);
    w.Key("to");   w.Int64(to);
    w.EndObject();
    return send_line(sb.GetString());
}

// ── SSH 터널 (Prism FlowClient 와 동일) ─────────────────────────────────────
bool StreamClient::ensure_tunnel() {
    if (tunnel_process_ != nullptr) {
        if (::WaitForSingleObject(tunnel_process_, 0) == WAIT_TIMEOUT) return true;
        ::CloseHandle(tunnel_process_);
        tunnel_process_ = nullptr;
    }
    if (tunnel_job_ == nullptr) {
        tunnel_job_ = ::CreateJobObjectW(nullptr, nullptr);
        if (tunnel_job_ != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(tunnel_job_, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
    }
    wchar_t cmd[640];
    ::swprintf_s(cmd,
        L"\"%s\" -N -o BatchMode=yes -o ExitOnForwardFailure=yes "
        L"-o ServerAliveInterval=30 -o ServerAliveCountMax=3 "
        L"-L %d:127.0.0.1:%d oracle", ssh_path().c_str(), kPort, kPort);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        wchar_t msg[320];
        ::swprintf_s(msg, L"SSH 실행 실패(오류 %lu): %s", ::GetLastError(), ssh_path().c_str());
        status(msg);
        return false;
    }
    if (tunnel_job_ != nullptr) ::AssignProcessToJobObject(tunnel_job_, pi.hProcess);
    ::CloseHandle(pi.hThread);
    tunnel_process_ = pi.hProcess;
    ::Sleep(1200);
    return true;
}

void StreamClient::close_tunnel() {
    if (tunnel_process_ != nullptr) {
        ::TerminateProcess(tunnel_process_, 0);
        ::CloseHandle(tunnel_process_);
        tunnel_process_ = nullptr;
    }
    if (tunnel_job_ != nullptr) {
        ::CloseHandle(tunnel_job_);
        tunnel_job_ = nullptr;
    }
}

// ── 소켓 ─────────────────────────────────────────────────────────────────────
bool StreamClient::connect_once() {
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = ::htons(static_cast<u_short>(kPort));
    ::inet_pton(AF_INET, kHost, &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::closesocket(s);
        return false;
    }
    DWORD timeout = 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::lock_guard<std::mutex> lk(sock_mutex_);
    sock_ = static_cast<unsigned long long>(s);
    return true;
}

void StreamClient::close_socket() {
    std::lock_guard<std::mutex> lk(sock_mutex_);
    if (sock_ != ~0ull) {
        ::closesocket(static_cast<SOCKET>(sock_));
        sock_ = ~0ull;
    }
}

bool StreamClient::send_line(const std::string& line) {
    std::lock_guard<std::mutex> lk(sock_mutex_);
    if (sock_ == ~0ull) return false;
    const std::string payload = line + "\n";
    int sent = 0;
    while (sent < static_cast<int>(payload.size())) {
        const int n = ::send(static_cast<SOCKET>(sock_), payload.data() + sent,
                             static_cast<int>(payload.size()) - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

// ── 수신 ─────────────────────────────────────────────────────────────────────
void StreamClient::handle_line(const std::string& line) {
    rapidjson::Document doc;
    if (doc.Parse(line.c_str()).HasParseError() || !doc.IsObject()) return;
    const std::string type = doc.HasMember("type") && doc["type"].IsString() ? doc["type"].GetString() : "";

    if (type == "ping" || type == "hello") return;

    if (type == "channels") {
        auto out = std::make_unique<std::vector<Channel>>();
        if (const auto it = doc.FindMember("channels"); it != doc.MemberEnd() && it->value.IsArray()) {
            out->reserve(it->value.Size());
            for (const auto& c : it->value.GetArray()) {
                if (!c.IsObject()) continue;
                Channel ch;
                ch.id         = json_wstr(c, "channel_id");
                ch.name       = json_wstr(c, "channel_name");
                ch.first_seen = json_i64(c, "first_seen");
                ch.last_seen  = json_i64(c, "last_seen");
                ch.samples    = static_cast<int>(json_i64(c, "samples"));
                ch.peak       = static_cast<int>(json_i64(c, "peak"));
                ch.current    = static_cast<int>(json_i64(c, "current"));
                if (!ch.id.empty()) out->push_back(std::move(ch));
            }
        }
        ChannelsFn fn;
        { std::lock_guard<std::mutex> lk(handler_mutex_); fn = on_channels_; }
        if (fn) fn(std::move(out));
        return;
    }

    if (type == "samples") {
        auto s = std::make_unique<Samples>();
        s->channel_id = json_wstr(doc, "channel_id");
        s->from = json_i64(doc, "from");
        s->to   = json_i64(doc, "to");
        if (const auto it = doc.FindMember("points"); it != doc.MemberEnd() && it->value.IsArray()) {
            s->points.reserve(it->value.Size());
            for (const auto& p : it->value.GetArray())
                if (p.IsArray() && p.Size() >= 2 && p[0].IsInt64() && p[1].IsInt())
                    s->points.push_back({p[0].GetInt64(), p[1].GetInt()});
        }
        if (const auto it = doc.FindMember("info"); it != doc.MemberEnd() && it->value.IsArray()) {
            for (const auto& p : it->value.GetArray()) {
                if (!p.IsArray() || p.Size() < 3 || !p[0].IsInt64()) continue;
                Info i;
                i.ts = p[0].GetInt64();
                if (p[1].IsString()) i.category = utf8_to_wide(p[1].GetString());
                if (p[2].IsString()) i.title    = utf8_to_wide(p[2].GetString());
                s->info.push_back(std::move(i));
            }
        }
        SamplesFn fn;
        { std::lock_guard<std::mutex> lk(handler_mutex_); fn = on_samples_; }
        if (fn) fn(std::move(s));
        return;
    }

    if (type == "error") status(L"서버 오류: " + json_wstr(doc, "message"));
    if (type == "bye")   status(L"서버 종료 — 재접속 예정");
}

void StreamClient::run() {
    std::string buf;
    while (running_.load()) {
        if (!ensure_tunnel()) {
            for (int i = 0; i < kReconnectMs / 100 && running_.load(); ++i) ::Sleep(100);
            continue;
        }
        if (!connect_once()) {
            status(L"서버 연결 대기 중...");
            for (int i = 0; i < kReconnectMs / 100 && running_.load(); ++i) ::Sleep(100);
            continue;
        }
        connected_ = true;
        status(L"연결됨");
        buf.clear();
        send_line("{\"type\":\"hello\",\"v\":1,\"id\":\"h1\"}");
        request_channels();

        char chunk[8192];
        while (running_.load()) {
            SOCKET s;
            { std::lock_guard<std::mutex> lk(sock_mutex_); s = static_cast<SOCKET>(sock_); }
            if (s == INVALID_SOCKET) break;
            const int n = ::recv(s, chunk, sizeof(chunk), 0);
            if (n == 0) break;
            if (n < 0) {
                if (::WSAGetLastError() == WSAETIMEDOUT) continue;
                break;
            }
            buf.append(chunk, static_cast<std::size_t>(n));
            for (std::size_t p; (p = buf.find('\n')) != std::string::npos;) {
                const std::string line = buf.substr(0, p);
                buf.erase(0, p + 1);
                if (!line.empty()) handle_line(line);
            }
        }
        connected_ = false;
        close_socket();
        if (running_.load()) {
            status(L"연결 끊김 — 재접속 중");
            for (int i = 0; i < kReconnectMs / 100 && running_.load(); ++i) ::Sleep(100);
        }
    }
    connected_ = false;
}

} // namespace sm
