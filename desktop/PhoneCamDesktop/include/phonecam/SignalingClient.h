#pragma once

#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <optional>
#include <nlohmann/json.hpp>

namespace phonecam {

// Forward declaration
class SignalingClient;

// Signaling message types matching Android protocol
enum class SignalingMessageType {
    HELLO,
    OFFER,
    ANSWER,
    CANDIDATE,
    COMMAND,
    PING,
    PONG,
    BYE,
    ERROR_MSG,
    STATUS,
    DISCONNECTED,
    UNKNOWN
};

// Parsed signaling message
struct SignalingMessage {
    SignalingMessageType type;
    nlohmann::json payload;  // Full JSON payload for flexible parsing
    
    // Helper to extract sessionId if present
    std::optional<std::string> getSessionId() const {
        if (payload.contains("sessionId") && payload["sessionId"].is_string()) {
            return payload["sessionId"].get<std::string>();
        }
        return std::nullopt;
    }
};

// Hello message data (from Android)
struct HelloMessage {
    std::string protocol;
    std::string sessionId;
    std::string deviceId;
    std::string name;
    std::string state;
    std::string camera;       // "back" or "front"
    bool micEnabled = true;
    int bitrateKbps = 5000;
    int width = 1280;
    int height = 720;
    int fps = 30;
};

// Offer message data
struct OfferMessage {
    std::string sessionId;
    std::string sdp;
};

// Answer message data
struct AnswerMessage {
    std::string sessionId;
    std::string sdp;
};

// ICE Candidate message data
struct CandidateMessage {
    std::string sessionId;
    std::string sdpMid;
    int sdpMLineIndex = 0;
    std::string candidate;
};

// Command message data (from Android)
struct CommandMessage {
    std::string sessionId;
    std::string action;       // setMic, setCamera, setBitrate, setCaptureMode, rename, disconnect, getStatus
    // Action-specific fields
    bool micEnabled = false;      // for setMic
    std::string camera;           // for setCamera: "back" or "front"
    int bitrateKbps = 0;          // for setBitrate
    std::string captureMode;      // for setCaptureMode
    std::string newName;          // for rename
};

// Error message data
struct ErrorMessage {
    std::string sessionId;
    std::string code;
    std::string message;
};

// Disconnected/Bye message data
struct DisconnectMessage {
    std::string sessionId;
    std::string reason;
};

// Status message data (from Android)
struct StatusMessage {
    std::string sessionId;
    std::string state;
    bool micEnabled = true;
    std::string camera;
    int bitrateKbps = 5000;
    int width = 1280;
    int height = 720;
    int fps = 30;
};

// Configuration for the signaling client
struct SignalingClientConfig {
    std::string host;                    // Hostname or IP address
    uint16_t port = 8080;                // Port
    std::string path = "/";              // WebSocket path (from TXT record "path")
    std::chrono::seconds connectTimeout{10};
    std::chrono::seconds pingInterval{25};
    std::chrono::seconds pongTimeout{10};
    bool autoReconnect = true;
    std::chrono::seconds reconnectDelay{5};
    size_t maxMessageSize = 1024 * 1024; // 1 MB
};

// Connection state
enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    RECONNECTING,
    ERROR_STATE
};

// Callback types
using OnHelloCallback = std::function<void(const HelloMessage& hello)>;
using OnOfferCallback = std::function<void(const OfferMessage& offer)>;
using OnAnswerCallback = std::function<void(const AnswerMessage& answer)>;
using OnCandidateCallback = std::function<void(const CandidateMessage& candidate)>;
using OnCommandCallback = std::function<void(const CommandMessage& command)>;
using OnErrorCallback = std::function<void(const ErrorMessage& error)>;
using OnDisconnectedCallback = std::function<void(const DisconnectMessage& disconnect)>;
using OnStatusCallback = std::function<void(const StatusMessage& status)>;
using OnConnectionStateChangeCallback = std::function<void(ConnectionState state, const std::string& error)>;

class SignalingClient {
public:
    SignalingClient();
    ~SignalingClient();

    // Non-copyable, movable
    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;
    SignalingClient(SignalingClient&&) = default;
    SignalingClient& operator=(SignalingClient&&) = default;

    // Connect to the Android WebSocket signaling server
    bool connect(const SignalingClientConfig& config);

    // Disconnect from the server
    void disconnect();

    // Check if connected
    bool isConnected() const { return state_.load() == ConnectionState::CONNECTED; }
    ConnectionState getState() const { return state_.load(); }

    // Send messages to Android
    bool sendAnswer(const std::string& sessionId, const std::string& sdp);
    bool sendCandidate(const std::string& sessionId, const std::string& sdpMid, int sdpMLineIndex, const std::string& candidate);
    bool sendCommand(const std::string& sessionId, const std::string& action, const nlohmann::json& params = {});
    bool sendPing();
    bool sendBye(const std::string& sessionId);
    
    // Helper command methods
    bool sendSetMicCommand(const std::string& sessionId, bool enabled);
    bool sendSetCameraCommand(const std::string& sessionId, const std::string& camera);  // "back" or "front"
    bool sendSetBitrateCommand(const std::string& sessionId, int bitrateKbps);
    bool sendSetCaptureModeCommand(const std::string& sessionId, const std::string& mode);
    bool sendRenameCommand(const std::string& sessionId, const std::string& newName);
    bool sendDisconnectCommand(const std::string& sessionId);
    bool sendGetStatusCommand(const std::string& sessionId);

    // Callbacks for incoming messages
    void setOnHelloCallback(OnHelloCallback callback) { onHelloCallback_ = std::move(callback); }
    void setOnOfferCallback(OnOfferCallback callback) { onOfferCallback_ = std::move(callback); }
    void setOnAnswerCallback(OnAnswerCallback callback) { onAnswerCallback_ = std::move(callback); }
    void setOnCandidateCallback(OnCandidateCallback callback) { onCandidateCallback_ = std::move(callback); }
    void setOnCommandCallback(OnCommandCallback callback) { onCommandCallback_ = std::move(callback); }
    void setOnErrorCallback(OnErrorCallback callback) { onErrorCallback_ = std::move(callback); }
    void setOnDisconnectedCallback(OnDisconnectedCallback callback) { onDisconnectedCallback_ = std::move(callback); }
    void setOnStatusCallback(OnStatusCallback callback) { onStatusCallback_ = std::move(callback); }
    void setOnConnectionStateChangeCallback(OnConnectionStateChangeCallback callback) { 
        onConnectionStateChangeCallback_ = std::move(callback); 
    }

    // Get config
    const SignalingClientConfig& getConfig() const { return config_; }

    // Process any pending events (call from main loop if needed)
    void processEvents();

private:
    // Internal implementation
    class Impl;
    std::unique_ptr<Impl> impl_;

    SignalingClientConfig config_;
    std::atomic<ConnectionState> state_{ConnectionState::DISCONNECTED};

    // Callbacks
    OnHelloCallback onHelloCallback_;
    OnOfferCallback onOfferCallback_;
    OnAnswerCallback onAnswerCallback_;
    OnCandidateCallback onCandidateCallback_;
    OnCommandCallback onCommandCallback_;
    OnErrorCallback onErrorCallback_;
    OnDisconnectedCallback onDisconnectedCallback_;
    OnStatusCallback onStatusCallback_;
    OnConnectionStateChangeCallback onConnectionStateChangeCallback_;

    // Helper to notify connection state change
    void notifyConnectionStateChange(ConnectionState newState, const std::string& error = "");
};

} // namespace phonecam