#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <optional>
#include <nlohmann/json.hpp>

// Include SessionManager for shared frame definitions
#include "SessionManager.h"

// Windows headers for named pipes
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#endif

namespace phonecam {

using json = nlohmann::json;

// Forward declarations
class SecurityManager;
class SessionManager;

// IPC Command types
enum class IpcCommand : uint32_t {
    GET_VIDEO_FRAME = 1,
    GET_AUDIO_FRAME = 2,
    GET_SESSION_INFO = 3,
    SUBSCRIBE = 4,
    UNSUBSCRIBE = 5,
    INVALID = 0xFFFFFFFF
};

// IPC Response status
enum class IpcStatus : uint32_t {
    OK = 0,
    ERR_INVALID_REQUEST = 1,
    ERR_UNAUTHORIZED = 2,
    ERR_SESSION_NOT_FOUND = 3,
    ERR_SESSION_INACTIVE = 4,
    ERR_NO_FRAME_AVAILABLE = 5,
    ERR_INTERNAL = 6,
    ERR_INVALID_TOKEN = 7,
    ERR_RATE_LIMITED = 8
};

// Request structure
struct IpcRequest {
    uint64_t id = 0;
    IpcCommand command = IpcCommand::INVALID;
    std::string sessionId;
    std::string token;
    json params;
};

// Response structure
struct IpcResponse {
    uint64_t id = 0;
    IpcStatus status = IpcStatus::ERR_INTERNAL;
    json data;
    std::string errorMessage;
};

// Session info
struct SessionInfo {
    std::string sessionId;
    std::string deviceId;
    bool videoEnabled = false;
    bool audioEnabled = false;
    bool muted = false;
    float gain = 1.0f;
    int audioDelayMs = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    uint32_t bitrateKbps = 0;
    std::string codec;
    bool connected = false;
};

// Callback function types
using VideoFrameCallback = std::function<std::optional<VideoFrame>(const std::string& sessionId)>;
using AudioFrameCallback = std::function<std::optional<AudioFrame>(const std::string& sessionId)>;
using SessionInfoCallback = std::function<std::optional<SessionInfo>(const std::string& sessionId)>;

// Push-based frame delivery callbacks
using VideoFramePushCallback = std::function<void(const std::string& sessionId, const VideoFrame& frame)>;
using AudioFramePushCallback = std::function<void(const std::string& sessionId, const AudioFrame& frame)>;

// IpcServer configuration
struct IpcServerConfig {
    std::string pipeNamePrefix = "PhoneCam_";  // Full pipe name: \\.\pipe\PhoneCam_<sessionId>
    uint32_t maxMessageSize = 16 * 1024 * 1024;  // 16 MB max message
    uint32_t maxPendingConnections = 10;
    uint32_t connectionTimeoutMs = 5000;
    uint32_t readTimeoutMs = 1000;
    uint32_t writeTimeoutMs = 1000;
};

// IpcServer class - Windows Named Pipe server with ACL
class IpcServer {
public:
    IpcServer(
        SecurityManager* securityManager,
        SessionManager* sessionManager,
        const IpcServerConfig& config = IpcServerConfig()
    );
    
    ~IpcServer();

    // Non-copyable, movable
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;
    IpcServer(IpcServer&&) noexcept;
    IpcServer& operator=(IpcServer&&) noexcept;

    // Start the IPC server
    bool start();

    // Stop the IPC server
    void stop();

    // Check if server is running
    bool isRunning() const;

    // Create a named pipe for a specific session
    // Returns the pipe name (e.g., \\.\pipe\PhoneCam_<sessionId>)
    std::string createSessionPipe(const std::string& sessionId);

    // Remove a session pipe
    void removeSessionPipe(const std::string& sessionId);

    // Get active session count
    size_t getActiveSessionCount() const;

    // Process pending messages (call periodically from main loop)
    void processMessages();

    // Deliver video frame to subscribed clients (push-based)
    void deliverVideoFrame(const std::string& sessionId, const VideoFrame& frame);

    // Deliver audio frame to subscribed clients (push-based)
    void deliverAudioFrame(const std::string& sessionId, const AudioFrame& frame);

    // Register push callbacks for real-time frame delivery
    void registerVideoPushCallback(VideoFramePushCallback callback);
    void registerAudioPushCallback(AudioFramePushCallback callback);
    void registerSessionInfoCallback(SessionInfoCallback callback);

private:
    // Internal pipe instance for a single session
    struct PipeInstance {
        HANDLE pipeHandle = INVALID_HANDLE_VALUE;
        std::string sessionId;
        OVERLAPPED readOverlapped{};
        OVERLAPPED writeOverlapped{};
        std::vector<uint8_t> readBuffer;
        std::vector<uint8_t> writeBuffer;
        uint32_t expectedMessageSize = 0;
        bool readingHeader = true;
        std::atomic<bool> active{false};
        std::thread workerThread;
    };
 
     // Security descriptor creation for ACL
     // Use a function object deleter to avoid function pointer type issues
     struct SecurityDescriptorDeleter {
         void operator()(SECURITY_DESCRIPTOR* p) const {
             if (p) {
                 LocalFree(p);
             }
         }
     };
     using SecurityDescriptorPtr = std::unique_ptr<SECURITY_DESCRIPTOR, SecurityDescriptorDeleter>;
     static SecurityDescriptorPtr createSecurityDescriptor();

    // Pipe server thread
    void serverThreadFunc(const std::string& sessionId);

    // Client connection handler
    void handleClientConnection(PipeInstance& instance);

    // Read a complete message (length-prefixed)
    bool readMessage(PipeInstance& instance, std::vector<uint8_t>& outMessage);

    // Write a complete message (length-prefixed)
    bool writeMessage(PipeInstance& instance, const std::vector<uint8_t>& message);

    // Process incoming request
    IpcResponse processRequest(const IpcRequest& request);

    // Handle specific commands
    IpcResponse handleGetVideoFrame(const IpcRequest& request);
    IpcResponse handleGetAudioFrame(const IpcRequest& request);
    IpcResponse handleGetSessionInfo(const IpcRequest& request);
    IpcResponse handleSubscribe(const IpcRequest& request);
    IpcResponse handleUnsubscribe(const IpcRequest& request);

    // Validate request token
    bool validateToken(const std::string& sessionId, const std::string& token) const;

    // Serialize response to JSON
    std::vector<uint8_t> serializeResponse(const IpcResponse& response);

    // Deserialize request from JSON
    std::optional<IpcRequest> deserializeRequest(const std::vector<uint8_t>& data);

    // Encode frame data to base64
    static std::string encodeBase64(const std::vector<uint8_t>& data);
    
    // Encode audio frame to base64
    static std::string encodeAudioBase64(const std::vector<float>& data);

    // Configuration
    IpcServerConfig config_;

    // Dependencies (non-owning pointers)
    SecurityManager* securityManager_ = nullptr;
    SessionManager* sessionManager_ = nullptr;

    // Push callbacks for real-time frame delivery
    VideoFramePushCallback videoPushCallback_;
    AudioFramePushCallback audioPushCallback_;
    SessionInfoCallback sessionInfoCallback_;
    mutable std::mutex pushCallbackMutex_;

    // Server state
    std::atomic<bool> running_{false};
    std::thread acceptorThread_;
    
    // Active pipe instances (sessionId -> PipeInstance)
    std::unordered_map<std::string, std::unique_ptr<PipeInstance>> pipes_;
    mutable std::mutex pipesMutex_;

    // Request ID counter
    std::atomic<uint64_t> requestCounter_{0};
};

} // namespace phonecam