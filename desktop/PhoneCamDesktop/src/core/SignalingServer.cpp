// SignalingServer.cpp
//
// Real blocking TCP signaling server speaking newline-delimited JSON, matching
// the protocol emitted by SignalingClient.kt on the Android side:
//
//   inbound  (phone -> desktop):
//     {"type":"sessionInit","sessionId":"...","deviceId":"...","payload":""}
//     {"type":"offer","sessionId":"...","sdp":"v=0\r\n..."}
//     {"type":"iceCandidate","sessionId":"...","candidate":"candidate:..."}
//     {"type":"sessionClose","sessionId":"..."}
//
//   outbound (desktop -> phone):
//     {"type":"answer","sessionId":"...","sdp":"v=0\r\n..."}
//     {"type":"iceCandidate","sessionId":"...","candidate":"candidate:..."}
//
// Notes:
//   - One reader thread per accepted connection (std::thread).
//   - Lines are terminated by '\n' (CRLF tolerated).
//   - Max line length: 256 KiB (config_.maxMessageSize).
//   - This milestone intentionally DOES NOT enforce authentication tokens to
//     match the unauthenticated session-init path of SignalingClient.kt. If
//     config_.requireAuthToken is true, we LOG a warning but accept anyway.

#include "phonecam/SignalingServer.h"

#include <iostream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#  define PHONECAM_INVALID_SOCKET INVALID_SOCKET
#  define PHONECAM_CLOSESOCKET(s) closesocket(s)
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
using socket_t = int;
#  define PHONECAM_INVALID_SOCKET (-1)
#  define PHONECAM_CLOSESOCKET(s) close(s)
#endif

namespace phonecam {

namespace {

// Hard cap on a single JSON line we will ever accept from the wire.
constexpr size_t kMaxLineBytes = 256 * 1024; // 256 KiB

// WsaInit: bring up Winsock once per process.
class WsaInit {
public:
    WsaInit() {
#ifdef _WIN32
        WSADATA wsa{};
        ok_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#else
        ok_ = true;
#endif
    }
    ~WsaInit() {
#ifdef _WIN32
        if (ok_) WSACleanup();
#endif
    }
    bool ok() const { return ok_; }
private:
    bool ok_ = false;
};

} // namespace

// Per-connection state.
struct Connection {
    socket_t sock = PHONECAM_INVALID_SOCKET;
    std::string deviceId;                       // learned from sessionInit
    std::vector<std::string> sessionIds;
    std::chrono::steady_clock::time_point lastActivity{};
    std::mutex writeMutex;                      // serialize sends on this socket
    std::atomic<bool> alive{true};
    uint32_t sequenceCounter = 0;
};

class SignalingServer::Impl {
public:
    Impl() = default;
    ~Impl() { stop(); }

    bool start(const SignalingServerConfig& config) {
        if (running_.load()) return false;
        config_ = config;

        static WsaInit wsaOnce;   // winsock init (process-lifetime)
        if (!wsaOnce.ok()) {
            std::cerr << "[SignalingServer] WSAStartup failed" << std::endl;
            return false;
        }

        listenSock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock_ == PHONECAM_INVALID_SOCKET) {
            std::cerr << "[SignalingServer] socket() failed" << std::endl;
            return false;
        }

        int yes = 1;
        ::setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.port);
        if (config_.bindAddress.empty() || config_.bindAddress == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            if (::inet_pton(AF_INET, config_.bindAddress.c_str(), &addr.sin_addr) != 1) {
                std::cerr << "[SignalingServer] invalid bindAddress: " << config_.bindAddress << std::endl;
                PHONECAM_CLOSESOCKET(listenSock_);
                listenSock_ = PHONECAM_INVALID_SOCKET;
                return false;
            }
        }

        if (::bind(listenSock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
            int e = WSAGetLastError();
            std::cerr << "[SignalingServer] bind() failed, WSA error " << e
                      << " on " << config_.bindAddress << ":" << config_.port << std::endl;
#else
            std::cerr << "[SignalingServer] bind() failed: " << std::strerror(errno)
                      << " on " << config_.bindAddress << ":" << config_.port << std::endl;
#endif
            PHONECAM_CLOSESOCKET(listenSock_);
            listenSock_ = PHONECAM_INVALID_SOCKET;
            return false;
        }

        if (::listen(listenSock_, 16) != 0) {
            std::cerr << "[SignalingServer] listen() failed" << std::endl;
            PHONECAM_CLOSESOCKET(listenSock_);
            listenSock_ = PHONECAM_INVALID_SOCKET;
            return false;
        }

        if (config_.requireAuthToken) {
            std::cout << "[SignalingServer] WARN: requireAuthToken is set but token"
                         " verification is NOT enforced in this milestone"
                         " (matches Android SignalingClient.kt bootstrap)."
                      << std::endl;
        }

        running_.store(true);
        return true;
    }

    bool isRunning() const { return running_.load(); }

    bool startAcceptThread() {
        if (!running_.load()) return false;
        acceptThread_ = std::thread(&Impl::acceptLoop, this);

        std::cout << "[SignalingServer] listening on "
                  << (config_.bindAddress.empty() ? "0.0.0.0" : config_.bindAddress)
                  << ":" << config_.port << std::endl;
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        // Unblock accept()
        if (listenSock_ != PHONECAM_INVALID_SOCKET) {
            PHONECAM_CLOSESOCKET(listenSock_);
            listenSock_ = PHONECAM_INVALID_SOCKET;
        }
        if (acceptThread_.joinable()) acceptThread_.join();

        // Close all connections and detach their reader threads.
        std::vector<std::thread> readersToJoin;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& c : connections_) {
                if (c->sock != PHONECAM_INVALID_SOCKET) {
                    c->alive.store(false);
#ifdef _WIN32
                    ::shutdown(c->sock, SD_BOTH);
#else
                    ::shutdown(c->sock, SHUT_RDWR);
#endif
                    PHONECAM_CLOSESOCKET(c->sock);
                    c->sock = PHONECAM_INVALID_SOCKET;
                }
            }
            connections_.clear();
            deviceIndex_.clear();
            readersToJoin = std::move(readerThreads_);
        }
        for (auto& t : readersToJoin) {
            if (t.joinable()) t.join();
        }
        std::cout << "[SignalingServer] stopped" << std::endl;
    }

    // ---------------- sends ----------------

    bool sendOffer(const std::string& sessionId, const std::string& deviceId,
                   const std::string& sdp) {
        nlohmann::json j;
        j["type"] = "offer";
        j["sessionId"] = sessionId;
        j["sdp"] = sdp;
        return sendJsonToDeviceSession(sessionId, deviceId, j.dump());
    }

    bool sendAnswer(const std::string& sessionId, const std::string& sdp) {
        nlohmann::json j;
        j["type"] = "answer";
        j["sessionId"] = sessionId;
        j["sdp"] = sdp;
        // Find owning connection via session index
        std::shared_ptr<Connection> conn = findConnBySession(sessionId);
        if (!conn) {
            std::cerr << "[SignalingServer] sendAnswer: no connection for session "
                      << sessionId << std::endl;
            return false;
        }
        bool result = sendLine(conn, j.dump());
        if (result) {
            std::cout << "[SignalingServer] Answer sent: sessionId=" << sessionId
                      << " sdpBytes=" << sdp.size() << std::endl;
        } else {
            std::cerr << "[SignalingServer] sendAnswer failed: sessionId=" << sessionId << std::endl;
        }
        return result;
    }

    bool sendIceCandidate(const std::string& sessionId, const std::string& sdpMid, int sdpMLineIndex, const std::string& candidate) {
        nlohmann::json j;
        j["type"] = "iceCandidate";
        j["sessionId"] = sessionId;
        j["sdpMid"] = sdpMid;
        j["sdpMLineIndex"] = sdpMLineIndex;
        j["candidate"] = candidate;
        std::shared_ptr<Connection> conn = findConnBySession(sessionId);
        if (!conn) {
            // Candidate may be produced before sessionInit completes registration;
            // this is normal. Log at low severity rather than dropping silently.
            std::cout << "[SignalingServer] sendIceCandidate: session not yet bound, "
                      << "dropping candidate for " << sessionId << std::endl;
            return false;
        }
        bool result = sendLine(conn, j.dump());
        if (result) {
            std::cout << "[SignalingServer] ICE candidate sent: sessionId=" << sessionId
                      << " mid=" << sdpMid << " mline=" << sdpMLineIndex
                      << " candidateBytes=" << candidate.size() << std::endl;
        } else {
            std::cerr << "[SignalingServer] sendIceCandidate failed: sessionId=" << sessionId << std::endl;
        }
        return result;
    }

    bool sendSessionClose(const std::string& sessionId) {
        nlohmann::json j;
        j["type"] = "sessionClose";
        j["sessionId"] = sessionId;
        std::shared_ptr<Connection> conn = findConnBySession(sessionId);
        if (!conn) return false;
        return sendLine(conn, j.dump());
    }

    bool sendError(const std::string& sessionId, const std::string& err) {
        nlohmann::json j;
        j["type"] = "error";
        j["sessionId"] = sessionId;
        j["error"] = err;
        std::shared_ptr<Connection> conn = findConnBySession(sessionId);
        if (!conn) {
            std::cerr << "[SignalingServer] sendError: no conn for session "
                      << sessionId << " err=" << err << std::endl;
            return false;
        }
        return sendLine(conn, j.dump());
    }

    void broadcast(const ServerSignalingMessage& /*msg*/) {
        // Not used in this milestone; single-device flows only.
    }

    size_t getConnectedDeviceCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deviceIndex_.size();
    }

    std::vector<std::string> getSessionsForDevice(const std::string& deviceId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = deviceIndex_.find(deviceId);
        if (it != deviceIndex_.end()) {
            return it->second->sessionIds;
        }
        return {};
    }

    void processEvents() {
        // No-op: work happens on accept-loop and per-connection reader threads.
    }

    // Callbacks (set by SignalingServer forwarding methods below)
    ServerOnOfferCallback onOfferCallback_;
    ServerOnAnswerCallback onAnswerCallback_;
    ServerOnIceCandidateCallback onIceCandidateCallback_;
    ServerOnSessionInitCallback onSessionInitCallback_;
    ServerOnSessionCloseCallback onSessionCloseCallback_;
    ServerOnErrorCallback onErrorCallback_;

private:
    void acceptLoop() {
        while (running_.load()) {
            sockaddr_in cli{};
#ifdef _WIN32
            int cliLen = sizeof(cli);
#else
            socklen_t cliLen = sizeof(cli);
#endif
            socket_t s = ::accept(listenSock_, reinterpret_cast<sockaddr*>(&cli), &cliLen);
            if (s == PHONECAM_INVALID_SOCKET) {
                if (!running_.load()) break;
#ifdef _WIN32
                // On Windows, when listening socket is closed from another
                // thread, accept returns WSAEINTR / WSAENOTSOCK.
                int e = WSAGetLastError();
                if (e == WSAEINTR || e == WSAENOTSOCK || e == WSAEINVAL) break;
#else
                if (errno == EINTR) continue;
#endif
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // Disable Nagle (low latency for ICE/SDP)
            int yes = 1;
            ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char*>(&yes), sizeof(yes));

            auto conn = std::make_shared<Connection>();
            conn->sock = s;
            conn->lastActivity = std::chrono::steady_clock::now();

            char ipbuf[INET_ADDRSTRLEN]{};
#ifdef _WIN32
            ::InetNtopA(AF_INET, &cli.sin_addr, ipbuf, sizeof(ipbuf));
#else
            ::inet_ntop(AF_INET, &cli.sin_addr, ipbuf, sizeof(ipbuf));
#endif
            std::cout << "[SignalingServer] Connection accepted from " << ipbuf
                      << ":" << ntohs(cli.sin_port) << std::endl;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                connections_.push_back(conn);
                readerThreads_.emplace_back(&Impl::readerLoop, this, conn);
            }
        }
    }

    void readerLoop(std::shared_ptr<Connection> conn) {
        // Read newline-delimited JSON. Each line is parsed independently and
        // dispatched synchronously on this thread.
        std::string buf;
        buf.reserve(4096);
        char chunk[4096];
        while (conn->alive.load() && running_.load()) {
            int n = ::recv(conn->sock, chunk, sizeof(chunk), 0);
            if (n <= 0) break;   // closed or error
            conn->lastActivity = std::chrono::steady_clock::now();
            buf.append(chunk, chunk + n);

            if (buf.size() > kMaxLineBytes && buf.find('\n') == std::string::npos) {
                std::cerr << "[SignalingServer] line exceeds " << kMaxLineBytes
                          << " bytes without newline; closing connection" << std::endl;
                break;
            }

            for (;;) {
                const size_t pos = buf.find('\n');
                if (pos == std::string::npos) break;
                std::string line = buf.substr(0, pos);
                // Strip trailing CR if present
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buf.erase(0, pos + 1);
                if (line.empty()) continue;
                if (line.size() > kMaxLineBytes) {
                    std::cerr << "[SignalingServer] dropping oversized line ("
                              << line.size() << " bytes)" << std::endl;
                    continue;
                }
                handleJsonLine(conn, line);
            }
        }

        conn->alive.store(false);
        if (conn->sock != PHONECAM_INVALID_SOCKET) {
            PHONECAM_CLOSESOCKET(conn->sock);
            conn->sock = PHONECAM_INVALID_SOCKET;
        }

        // Fire session-close for all sessions on this connection
        std::vector<std::string> toClose;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            toClose = conn->sessionIds;
            if (!conn->deviceId.empty()) {
                deviceIndex_.erase(conn->deviceId);
            }
            connections_.erase(
                std::remove_if(connections_.begin(), connections_.end(),
                               [&](const std::shared_ptr<Connection>& c) { return c.get() == conn.get(); }),
                connections_.end());
        }
        for (const auto& sid : toClose) {
            if (onSessionCloseCallback_) onSessionCloseCallback_(sid);
        }
        std::cout << "[SignalingServer] Connection closed: device="
                  << (conn->deviceId.empty() ? std::string("<uninit>") : conn->deviceId)
                  << " sessions=" << conn->sessionIds.size() << std::endl;
    }

    void handleJsonLine(const std::shared_ptr<Connection>& conn, const std::string& line) {
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(line);
        } catch (const std::exception& e) {
            std::cerr << "[SignalingServer] JSON parse error: " << e.what() << std::endl;
            if (onErrorCallback_) onErrorCallback_("", "json-parse");
            return;
        }

        const std::string type = j.value("type", "");
        const std::string sessionId = j.value("sessionId", "");

        if (type == "sessionInit") {
            const std::string deviceId = j.value("deviceId", "");
            if (deviceId.empty() || sessionId.empty()) return;
            conn->deviceId = deviceId;
            if (std::find(conn->sessionIds.begin(), conn->sessionIds.end(), sessionId)
                    == conn->sessionIds.end()) {
                conn->sessionIds.push_back(sessionId);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                deviceIndex_[deviceId] = conn;
            }
            std::cout << "[SignalingServer] sessionInit: deviceId=" << deviceId
                      << " sessionId=" << sessionId << std::endl;
            if (onSessionInitCallback_) onSessionInitCallback_(sessionId, deviceId);
            return;
        }

        if (type == "offer") {
            const std::string sdp = j.value("sdp", "");
            if (sessionId.empty() || sdp.empty()) return;
            // sessionInit usually precedes offer. If conn->deviceId is still
            // empty, resist crashing; just log and treat deviceId as unknown.
            const std::string deviceId = conn->deviceId.empty()
                ? j.value("deviceId", std::string(""))
                : conn->deviceId;
            if (std::find(conn->sessionIds.begin(), conn->sessionIds.end(), sessionId)
                    == conn->sessionIds.end()) {
                conn->sessionIds.push_back(sessionId);
            }
            std::cout << "[SignalingServer] offer received: sessionId=" << sessionId
                      << " deviceId=" << deviceId << " sdpBytes=" << sdp.size() << std::endl;
            if (onOfferCallback_) onOfferCallback_(sessionId, deviceId, sdp);
            return;
        }

        if (type == "iceCandidate") {
            const std::string c = j.value("candidate", "");
            const std::string sdpMid = j.value("sdpMid", "");
            const int sdpMLineIndex = j.value("sdpMLineIndex", 0);
            if (sessionId.empty() || c.empty()) return;
            std::cout << "[SignalingServer] ICE candidate received: sessionId=" << sessionId
                      << " mid=" << sdpMid << " mline=" << sdpMLineIndex
                      << " candidateBytes=" << c.size() << std::endl;
            if (onIceCandidateCallback_) onIceCandidateCallback_(sessionId, sdpMid, sdpMLineIndex, c);
            return;
        }

        if (type == "sessionClose") {
            if (!sessionId.empty()) {
                std::cout << "[SignalingServer] sessionClose: sessionId=" << sessionId << std::endl;
                if (onSessionCloseCallback_) onSessionCloseCallback_(sessionId);
            }
            return;
        }

        if (type == "ping") {
            conn->lastActivity = std::chrono::steady_clock::now();
            nlohmann::json pong;
            pong["type"] = "pong";
            pong["sessionId"] = sessionId;
            sendLine(conn, pong.dump());
            return;
        }

        std::cerr << "[SignalingServer] unknown message type: " << type
                  << " sessionId=" << sessionId << std::endl;
        if (onErrorCallback_) onErrorCallback_(sessionId, "unknown-type:" + type);
    }

    bool sendLine(const std::shared_ptr<Connection>& conn, const std::string& jsonLine) {
        if (!conn || !conn->alive.load()) return false;
        std::string line = jsonLine;
        line.push_back('\n');
        std::lock_guard<std::mutex> wl(conn->writeMutex);
        // Send with retry until all bytes written (bounded by socket errors)
        size_t sent = 0;
        while (sent < line.size()) {
            int n = ::send(conn->sock, line.data() + sent,
                           static_cast<int>(line.size() - sent), 0);
            if (n <= 0) {
                std::cerr << "[SignalingServer] send() failed; closing connection" << std::endl;
                conn->alive.store(false);
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool sendJsonToDeviceSession(const std::string& sessionId, const std::string& deviceId,
                                 const std::string& jsonLine) {
        std::shared_ptr<Connection> conn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = deviceIndex_.find(deviceId);
            if (it != deviceIndex_.end()) conn = it->second;
        }
        if (!conn) conn = findConnBySession(sessionId);
        if (!conn) return false;
        return sendLine(conn, jsonLine);
    }

    std::shared_ptr<Connection> findConnBySession(const std::string& sessionId) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& c : connections_) {
            if (std::find(c->sessionIds.begin(), c->sessionIds.end(), sessionId)
                    != c->sessionIds.end()) {
                return c;
            }
        }
        return nullptr;
    }

    SignalingServerConfig config_{};
    std::atomic<bool> running_{false};
    socket_t listenSock_ = PHONECAM_INVALID_SOCKET;
    std::thread acceptThread_;

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Connection>> connections_;
    std::unordered_map<std::string, std::shared_ptr<Connection>> deviceIndex_;
    std::vector<std::thread> readerThreads_;
};

// ---------------- SignalingServer forwarding ----------------

SignalingServer::SignalingServer() : impl_(std::make_unique<Impl>()) {}
SignalingServer::~SignalingServer() = default;

bool SignalingServer::start(const SignalingServerConfig& config) {
    config_ = config;
    // Bridge our stored callbacks into Impl before starting the accept loop
    // so no race exists between "callback registered" and "first request
    // dispatched".
    impl_->onOfferCallback_          = onOfferCallback_;
    impl_->onAnswerCallback_         = onAnswerCallback_;
    impl_->onIceCandidateCallback_   = onIceCandidateCallback_;
    impl_->onSessionInitCallback_    = onSessionInitCallback_;
    impl_->onSessionCloseCallback_   = onSessionCloseCallback_;
    impl_->onErrorCallback_          = onErrorCallback_;

    bool ok = impl_->start(config);
    if (ok) {
        ok = impl_->startAcceptThread();
    }
    running_.store(ok);
    return ok;
}

void SignalingServer::broadcast(const ServerSignalingMessage& msg) {
    impl_->broadcast(msg);
}

void SignalingServer::stop() {
    impl_->stop();
    running_.store(false);
}

bool SignalingServer::sendOffer(const std::string& sessionId, const std::string& deviceId, const std::string& sdp) {
    return impl_->sendOffer(sessionId, deviceId, sdp);
}
bool SignalingServer::sendAnswer(const std::string& sessionId, const std::string& sdp) {
    return impl_->sendAnswer(sessionId, sdp);
}
bool SignalingServer::sendIceCandidate(const std::string& sessionId, const std::string& sdpMid, int sdpMLineIndex, const std::string& candidate) {
    return impl_->sendIceCandidate(sessionId, sdpMid, sdpMLineIndex, candidate);
}
bool SignalingServer::sendSessionClose(const std::string& sessionId) {
    return impl_->sendSessionClose(sessionId);
}
bool SignalingServer::sendError(const std::string& sessionId, const std::string& error) {
    return impl_->sendError(sessionId, error);
}
size_t SignalingServer::getConnectedDeviceCount() const {
    return impl_->getConnectedDeviceCount();
}
std::vector<std::string> SignalingServer::getSessionsForDevice(const std::string& deviceId) const {
    return impl_->getSessionsForDevice(deviceId);
}
void SignalingServer::processEvents() {
    impl_->processEvents();
}

// isRunning is declared inline in the header for the atomic member;
// the real source of truth lives in Impl. We keep them in sync via start()/stop().

} // namespace phonecam
