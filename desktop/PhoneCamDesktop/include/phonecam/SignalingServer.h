#pragma once

#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace phonecam {

// Forward declaration for WebSocket connection
class WebSocketConnection;

// Server-side signaling message types (distinct from client-side)
enum class ServerSignalingMessageType : uint8_t {
    OFFER = 0,
    ANSWER = 1,
    ICE_CANDIDATE = 2,
    SESSION_INIT = 3,
    SESSION_CLOSE = 4,
    SIGNAL_ERROR = 5,
    PING = 6,
    PONG = 7
};

struct ServerSignalingMessage {
    ServerSignalingMessageType type;
    std::string sessionId;
    std::string deviceId;
    std::string payload; // JSON payload (SDP, ICE candidate, etc.)
    uint64_t timestamp = 0;
    uint32_t sequence = 0;
};

// Callback types for signaling events
using ServerOnOfferCallback = std::function<void(const std::string& sessionId, const std::string& deviceId, const std::string& sdp)>;
using ServerOnAnswerCallback = std::function<void(const std::string& sessionId, const std::string& sdp)>;
using ServerOnIceCandidateCallback = std::function<void(const std::string& sessionId, const std::string& sdpMid, int sdpMLineIndex, const std::string& candidate)>;
using ServerOnSessionInitCallback = std::function<void(const std::string& sessionId, const std::string& deviceId)>;
using ServerOnSessionCloseCallback = std::function<void(const std::string& sessionId)>;
using ServerOnErrorCallback = std::function<void(const std::string& sessionId, const std::string& error)>;

struct SignalingServerConfig {
    uint16_t port = 8080;
    // Bind to loopback by default so the unauthenticated bootstrap channel
    // is not exposed to the LAN. Set to "0.0.0.0" only when an auth token
    // (requireAuthToken / PHONECAM_SIGNALING_TOKEN) and pairing are in place.
    std::string bindAddress = "127.0.0.1";
    size_t maxConnections = 100;
    size_t maxMessageSize = 1024 * 1024; // 1 MB
    bool enablePingPong = true;
    std::chrono::seconds pingInterval{30};
    std::chrono::seconds connectionTimeout{60};

    // When true, incoming connections must present an authentication token
    // before any session message is processed; connections that fail to
    // authenticate within connectionTimeout are closed. Pairing of the token
    // with SecurityManager happens at the application layer.
    bool requireAuthToken = false;
};

class SignalingServer {
public:
    SignalingServer();
    ~SignalingServer();

    // Initialize and start the server
    bool start(const SignalingServerConfig& config = {});
    
    // Stop the server
    void stop();

    // Check if server is running
    bool isRunning() const { return running_.load(); }

    // Get server configuration
    const SignalingServerConfig& getConfig() const { return config_; }

    // Callbacks for incoming signaling messages
    void setOnOfferCallback(ServerOnOfferCallback callback) { onOfferCallback_ = std::move(callback); }
    void setOnAnswerCallback(ServerOnAnswerCallback callback) { onAnswerCallback_ = std::move(callback); }
    void setOnIceCandidateCallback(ServerOnIceCandidateCallback callback) { onIceCandidateCallback_ = std::move(callback); }
    void setOnSessionInitCallback(ServerOnSessionInitCallback callback) { onSessionInitCallback_ = std::move(callback); }
    void setOnSessionCloseCallback(ServerOnSessionCloseCallback callback) { onSessionCloseCallback_ = std::move(callback); }
    void setOnErrorCallback(ServerOnErrorCallback callback) { onErrorCallback_ = std::move(callback); }

    // Send signaling message to a specific session/device
    bool sendOffer(const std::string& sessionId, const std::string& deviceId, const std::string& sdp);
    bool sendAnswer(const std::string& sessionId, const std::string& sdp);
    bool sendIceCandidate(const std::string& sessionId, const std::string& sdpMid, int sdpMLineIndex, const std::string& candidate);
    bool sendSessionClose(const std::string& sessionId);
    bool sendError(const std::string& sessionId, const std::string& error);

    // Broadcast message to all connected devices
    void broadcast(const ServerSignalingMessage& message);

    // Get connected device count
    size_t getConnectedDeviceCount() const;

    // Get session IDs for a device
    std::vector<std::string> getSessionsForDevice(const std::string& deviceId) const;

    // Process pending operations (call periodically from main loop)
    void processEvents();

private:
    // Internal implementation
    class Impl;
    std::unique_ptr<Impl> impl_;

    SignalingServerConfig config_;
    std::atomic<bool> running_{false};

    // Callbacks
    ServerOnOfferCallback onOfferCallback_;
    ServerOnAnswerCallback onAnswerCallback_;
    ServerOnIceCandidateCallback onIceCandidateCallback_;
    ServerOnSessionInitCallback onSessionInitCallback_;
    ServerOnSessionCloseCallback onSessionCloseCallback_;
    ServerOnErrorCallback onErrorCallback_;
};

} // namespace phonecam