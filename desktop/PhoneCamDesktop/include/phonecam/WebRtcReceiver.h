#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <queue>
#include <atomic>
#include <thread>
#include <condition_variable>

namespace phonecam {

// Video frame from WebRTC
struct WebRtcVideoFrame {
    std::vector<uint8_t> data;
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t timestampUs = 0;
    uint32_t stride = 0;
    uint32_t format = 0; // I420=0, NV12=1, RGBA=2
    std::string codec; // H264, VP8, VP9, AV1
};

// Audio frame from WebRTC
struct WebRtcAudioFrame {
    std::vector<float> data; // Interleaved samples (PCM) or encoded Opus payload
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    int64_t timestampUs = 0;
    uint32_t frames = 0;      // 0 = raw Opus bitstream (first 4 bytes = payload size)
    std::string codec;        // "opus" or "PCM"
};

// Session configuration
struct WebRtcSessionConfig {
    std::string sessionId;
    std::string deviceId;
    std::string remoteSdp;
    std::string localSdp;
    bool videoEnabled = true;
    bool audioEnabled = true;
    bool dtlsEnabled = true;
    bool srtpEnabled = true;
};

// Callback types
using WebRtcVideoFrameCallback = std::function<void(const std::string& sessionId, const WebRtcVideoFrame& frame)>;
using WebRtcAudioFrameCallback = std::function<void(const std::string& sessionId, const WebRtcAudioFrame& frame)>;
using SessionStateCallback = std::function<void(const std::string& sessionId, const std::string& state)>;
using IceCandidateCallback = std::function<void(const std::string& sessionId, const std::string& sdpMid, int sdpMLineIndex, const std::string& candidate)>;

// WebRTC Receiver interface
class WebRtcReceiver {
public:
    virtual ~WebRtcReceiver() = default;

    // Initialize WebRTC factory and decoders
    virtual bool initialize() = 0;

    // Create a new session with remote SDP offer
    virtual bool createSession(const WebRtcSessionConfig& config) = 0;

    // Remove a session and cleanup resources
    virtual bool removeSession(const std::string& sessionId) = 0;

    // Set remote answer for pending offer (desktop-as-offerer; not used in PhoneCam's
    // current architecture where the ANDROID phone is always the offerer and the
    // DESKTOP is the answerer -- kept for interface completeness).
    virtual bool setRemoteAnswer(const std::string& sessionId, const std::string& remoteSdp) = 0;

    // Set remote SDP offer (desktop is the ANSWERER). After this call,
    // libdatachannel produces the local answer, accessible via getLocalAnswer().
    // This is the PRIMARY SDP entry point for PhoneCam: Android always offers,
    // desktop always answers. Do NOT overload setRemoteAnswer() to mean "set offer";
    // SDP direction must be explicit and unambiguous.
    virtual bool setRemoteOffer(const std::string& sessionId, const std::string& sdp) = 0;

    // Add ICE candidate
    virtual bool addIceCandidate(const std::string& sessionId, const std::string& sdpMid, int sdpMLineIndex, const std::string& candidate) = 0;

    // Set video frame callback
    virtual void setVideoFrameCallback(WebRtcVideoFrameCallback callback) = 0;

    // Set audio frame callback
    virtual void setAudioFrameCallback(WebRtcAudioFrameCallback callback) = 0;

    // Set session state callback
    virtual void setSessionStateCallback(SessionStateCallback callback) = 0;

    // Set ICE candidate callback (for sending to remote)
    virtual void setIceCandidateCallback(IceCandidateCallback callback) = 0;

    // Get local SDP offer for a session
    virtual std::optional<std::string> getLocalOffer(const std::string& sessionId) = 0;

    // Get local SDP answer for a session
    virtual std::optional<std::string> getLocalAnswer(const std::string& sessionId) = 0;

    // Get session state
    virtual std::string getSessionState(const std::string& sessionId) = 0;

    // Enable/disable video for session
    virtual bool setVideoEnabled(const std::string& sessionId, bool enabled) = 0;

    // Enable/disable audio for session
    virtual bool setAudioEnabled(const std::string& sessionId, bool enabled) = 0;

    // Check if session exists
    virtual bool hasSession(const std::string& sessionId) const = 0;

    // Get all session IDs
    virtual std::vector<std::string> getAllSessionIds() const = 0;

    // Shutdown all sessions and cleanup
    virtual void shutdown() = 0;
};

// Factory function to create WebRtcReceiver implementation
std::unique_ptr<WebRtcReceiver> createWebRtcReceiver();

} // namespace phonecam