#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <functional>
#include <mutex>
#include <memory>

namespace phonecam {

// Forward declaration
class WebRtcReceiver;

// Video frame structure for frame retrieval
struct VideoFrame {
    std::vector<uint8_t> data;
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t timestampUs = 0;
    uint32_t stride = 0;
    uint32_t format = 0; // e.g., I420=0, NV12=1, RGBA=2
    std::string codec;
};

// Audio frame structure for frame retrieval
struct AudioFrame {
    std::vector<float> data; // Planar: interleaved stereo samples
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    int64_t timestampUs = 0;
    uint32_t frames = 0;
};

struct CameraSession {
    std::string deviceId;
    std::string sessionId;
    bool videoEnabled = true;
    bool audioEnabled = true;
    bool muted = false;
    float gain = 1.0f;
    int audioDelayMs = 0;
    
    // Runtime state
    std::string state = "new"; // new, connecting, connected, disconnected, error
    std::string videoCodec;
    std::string audioCodec;
    uint32_t videoWidth = 0;
    uint32_t videoHeight = 0;
    uint32_t videoFps = 0;
    uint32_t videoBitrate = 0;
    int64_t lastFrameTimestampUs = 0;
    int64_t lastAudioTimestampUs = 0;
};

// Callback types for frame retrieval
using VideoFrameCallback = std::function<std::optional<VideoFrame>(const std::string& sessionId)>;
using AudioFrameCallback = std::function<std::optional<AudioFrame>(const std::string& sessionId)>;

// Consumer callbacks for real-time frame distribution
using VideoFrameConsumer = std::function<void(const std::string& sessionId, const VideoFrame& frame)>;
using AudioFrameConsumer = std::function<void(const std::string& sessionId, const AudioFrame& frame)>;

class SessionManager {
public:
    SessionManager();
    ~SessionManager();

    // Non-copyable, movable
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;
    SessionManager(SessionManager&&) noexcept = default;
    SessionManager& operator=(SessionManager&&) noexcept = default;

    // Initialize WebRTC receiver
    bool initialize();

    // Session management
    bool addSession(const CameraSession& session);
    bool removeSession(const std::string& sessionId);
    
    // Session property access
    bool setAudioEnabled(const std::string& sessionId, bool enabled);
    bool setMuted(const std::string& sessionId, bool muted);
    bool setVideoEnabled(const std::string& sessionId, bool enabled);
    bool setGain(const std::string& sessionId, float gain);
    bool setAudioDelay(const std::string& sessionId, int delayMs);
    
    // Session query
    std::optional<CameraSession> getSession(const std::string& sessionId) const;
    bool hasSession(const std::string& sessionId) const;
    std::vector<std::string> getAllSessionIds() const;

    // Frame retrieval callbacks (set by video/audio pipeline)
    void setVideoFrameCallback(VideoFrameCallback callback);
    void setAudioFrameCallback(AudioFrameCallback callback);
    
    // Frame retrieval (uses callbacks if set)
    std::optional<VideoFrame> getVideoFrame(const std::string& sessionId);
    std::optional<AudioFrame> getAudioFrame(const std::string& sessionId);

    // Real-time consumer registration (for OBS, preview UI, etc.)
    void registerVideoConsumer(VideoFrameConsumer consumer);
    void registerAudioConsumer(AudioFrameConsumer consumer);
    void unregisterVideoConsumer();
    void unregisterAudioConsumer();

    // WebRTC signaling integration
    bool setRemoteAnswer(const std::string& sessionId, const std::string& remoteSdp);
    bool addIceCandidate(const std::string& sessionId, const std::string& candidate);
    std::optional<std::string> getLocalOffer(const std::string& sessionId);
    std::optional<std::string> getLocalAnswer(const std::string& sessionId);
    std::string getSessionState(const std::string& sessionId) const;

    // Shutdown
    void shutdown();

private:
    // Internal frame delivery to consumers
    void deliverVideoFrame(const std::string& sessionId, const VideoFrame& frame);
    void deliverAudioFrame(const std::string& sessionId, const AudioFrame& frame);

    // WebRTC receiver
    std::unique_ptr<WebRtcReceiver> webRtcReceiver_;
    
    // Session storage
    mutable std::mutex sessionsMutex_;
    std::unordered_map<std::string, CameraSession> sessions_;
    
    // Frame callbacks (legacy pull-based)
    VideoFrameCallback videoFrameCallback_;
    AudioFrameCallback audioFrameCallback_;
    
    // Real-time consumers (push-based)
    VideoFrameConsumer videoConsumer_;
    AudioFrameConsumer audioConsumer_;
    mutable std::mutex consumerMutex_;
    
    bool initialized_ = false;
};

} // namespace phonecam