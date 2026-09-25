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

// Signaling message types
enum class SignalingMessageType : uint8_t {
    OFFER = 0,
    ANSWER = 1,
    ICE_CANDIDATE = 2,
    SESSION_INIT = 3,
    SESSION_CLOSE = 4,
    SIGNAL_ERROR = 5,
    PING = 6,
    PONG = 7
};

struct SignalingMessage {
    SignalingMessageType type;
    std::string sessionId;
    std::string deviceId;
    std::string payload; // JSON payload (SDP, ICE candidate, etc.)
    uint64_t timestamp = 0;
    uint32_t sequence = 0;
};

// Callback types for signaling events
using OnOfferCallback = std::function<void(const std::string& sessionId, const std::string& deviceId, const std::string& sdp)>;
using OnAnswerCallback = std::function<void(const std::string& sessionId, const std::string& sdp)>;
using OnIceCandidateCallback = std::function<void(const std::string& sessionId, const std::string& candidate)>;
using OnSessionInitCallback = std::function<void(const std::string& sessionId, const std::string& deviceId)>;
using OnSessionCloseCallback = std::function<void(const std::string& sessionId)>;
using OnErrorCallback = std::function<void(const std::string& sessionId, const std::string& error)>;

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
    void setOnOfferCallback(OnOfferCallback callback) { onOfferCallback_ = std::move(callback); }
    void setOnAnswerCallback(OnAnswerCallback callback) { onAnswerCallback_ = std::move(callback); }
    void setOnIceCandidateCallback(OnIceCandidateCallback callback) { onIceCandidateCallback_ = std::move(callback); }
    void setOnSessionInitCallback(OnSessionInitCallback callback) { onSessionInitCallback_ = std::move(callback); }
    void setOnSessionCloseCallback(OnSessionCloseCallback callback) { onSessionCloseCallback_ = std::move(callback); }
    void setOnErrorCallback(OnErrorCallback callback) { onErrorCallback_ = std::move(callback); }

    // Send signaling message to a specific session/device
    bool sendOffer(const std::string& sessionId, const std::string& deviceId, const std::string& sdp);
    bool sendAnswer(const std::string& sessionId, const std::string& sdp);
    bool sendIceCandidate(const std::string& sessionId, const std::string& candidate);
    bool sendSessionClose(const std::string& sessionId);
    bool sendError(const std::string& sessionId, const std::string& error);

    // Broadcast message to all connected devices
    void broadcast(const SignalingMessage& message);

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
    OnOfferCallback onOfferCallback_;
    OnAnswerCallback onAnswerCallback_;
    OnIceCandidateCallback onIceCandidateCallback_;
    OnSessionInitCallback onSessionInitCallback_;
    OnSessionCloseCallback onSessionCloseCallback_;
    OnErrorCallback onErrorCallback_;
};

} // namespace phonecam