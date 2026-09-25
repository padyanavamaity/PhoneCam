#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <nlohmann/json.hpp>

// Platform-specific headers
#ifdef _WIN32
// WIN32_LEAN_AND_MEAN and NOMINMAX are defined via CMake target_compile_definitions
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#endif

namespace phonecam_obs {

using json = nlohmann::json;

// IPC Command types (must match server)
enum class IpcCommand : uint32_t {
    GET_VIDEO_FRAME = 1,
    GET_AUDIO_FRAME = 2,
    GET_SESSION_INFO = 3,
    SUBSCRIBE = 4,
    UNSUBSCRIBE = 5,
    INVALID = 0xFFFFFFFF
};

// IPC Response status (must match server)
// Note: Prefixed with STATUS_ to avoid conflicts with Windows macros
// (windows.h defines ERROR_INVALID_TOKEN and potentially other ERROR_* macros)
enum class IpcStatus : uint32_t {
    STATUS_OK = 0,
    STATUS_INVALID_REQUEST = 1,
    STATUS_UNAUTHORIZED = 2,
    STATUS_SESSION_NOT_FOUND = 3,
    STATUS_SESSION_INACTIVE = 4,
    STATUS_NO_FRAME_AVAILABLE = 5,
    STATUS_INTERNAL = 6,
    STATUS_INVALID_TOKEN = 7,
    STATUS_RATE_LIMITED = 8
};

// Video frame data
struct VideoFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    std::string format;  // "I420", "NV12", "RGBA", etc.
    uint64_t timestampUs = 0;
    std::vector<uint8_t> data;  // Raw frame data
};

// Audio frame data
struct AudioFrame {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t samplesPerChannel = 0;
    std::string format;  // "float32", "int16", etc.
    uint64_t timestampUs = 0;
    std::vector<float> data;  // Interleaved float samples
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
    IpcStatus status = IpcStatus::STATUS_INTERNAL;
    json data;
    std::string errorMessage;
};

// IpcClient configuration
struct IpcClientConfig {
    std::string pipeNamePrefix = "PhoneCam_";  // Full pipe name: \\.\pipe\PhoneCam_<sessionId>
    uint32_t connectTimeoutMs = 5000;
    uint32_t readTimeoutMs = 1000;
    uint32_t writeTimeoutMs = 1000;
    uint32_t maxMessageSize = 16 * 1024 * 1024;  // 16 MB
};

// IpcClient class - Named pipe client for OBS plugin
class IpcClient {
public:
    IpcClient(const IpcClientConfig& config = IpcClientConfig());
    ~IpcClient();

    // Non-copyable, movable
    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;
    IpcClient(IpcClient&&) noexcept;
    IpcClient& operator=(IpcClient&&) noexcept;

    // Connect to a session's named pipe
    // pipeName format: \\.\pipe\PhoneCam_<sessionId> (Windows) or /tmp/PhoneCam_<sessionId> (Linux)
    bool connect(const std::string& pipeName);

    // Disconnect from the pipe
    void disconnect();

    // Check if connected
    bool isConnected() const;

    // Set session ID and token for subsequent requests
    void setSession(const std::string& sessionId, const std::string& token);

    // Synchronous request/response
    std::optional<IpcResponse> sendRequest(IpcCommand command, const json& params = json::object());

    // Convenience methods
    std::optional<VideoFrame> getVideoFrame();
    std::optional<AudioFrame> getAudioFrame();
    std::optional<SessionInfo> getSessionInfo();
    bool subscribe();
    bool unsubscribe();

    // Get last error
    std::string getLastError() const;

private:
    // Platform-specific handle
#ifdef _WIN32
    using PipeHandle = HANDLE;
    static constexpr PipeHandle INVALID_PIPE_HANDLE = INVALID_HANDLE_VALUE;
#else
    using PipeHandle = int;
    static constexpr PipeHandle INVALID_PIPE_HANDLE = -1;
#endif

    // Platform-specific connection
    bool connectPlatform(const std::string& pipeName);
    void disconnectPlatform();
    bool writePlatform(const std::vector<uint8_t>& data);
    bool readPlatform(std::vector<uint8_t>& outData, uint32_t expectedSize);
    bool waitForReadable(uint32_t timeoutMs);
    bool waitForWritable(uint32_t timeoutMs);

    // Common functionality
    std::vector<uint8_t> serializeRequest(const IpcRequest& request);
    std::optional<IpcResponse> deserializeResponse(const std::vector<uint8_t>& data);
    std::optional<VideoFrame> parseVideoFrame(const json& data);
    std::optional<AudioFrame> parseAudioFrame(const json& data);
    std::optional<SessionInfo> parseSessionInfo(const json& data);
    static std::vector<uint8_t> base64Decode(const std::string& data);
    static std::vector<float> base64DecodeFloat(const std::string& data);

    // Configuration
    IpcClientConfig config_;

    // Connection state
    PipeHandle pipeHandle_ = INVALID_PIPE_HANDLE;
    std::string currentSessionId_;
    std::string currentToken_;
    std::atomic<uint64_t> requestCounter_{0};
    mutable std::mutex mutex_;
    std::string lastError_;
    
    // Connection flag
    std::atomic<bool> connected_{false};
};

} // namespace phonecam_obs