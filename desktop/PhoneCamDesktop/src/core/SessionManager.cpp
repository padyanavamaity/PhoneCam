#include "phonecam/SessionManager.h"
#include "phonecam/WebRtcReceiver.h"
#include "phonecam/SecurityManager.h"

#include <iostream>
#include <algorithm>
#include <chrono>

namespace phonecam {

SessionManager::SessionManager() = default;

SessionManager::~SessionManager() {
    shutdown();
}

bool SessionManager::initialize() {
    if (initialized_) {
        return true;
    }

    // Create WebRTC receiver
    webRtcReceiver_ = createWebRtcReceiver();
    if (!webRtcReceiver_) {
        std::cerr << "[SessionManager] Failed to create WebRtcReceiver" << std::endl;
        return false;
    }

    // Initialize WebRTC
    if (!webRtcReceiver_->initialize()) {
        std::cerr << "[SessionManager] Failed to initialize WebRTC" << std::endl;
        webRtcReceiver_.reset();
        return false;
    }

    // Set up WebRTC callbacks
    webRtcReceiver_->setVideoFrameCallback(
        [this](const std::string& sessionId, const WebRtcVideoFrame& frame) {
            // Convert WebRTC frame to SessionManager frame
            VideoFrame videoFrame;
            videoFrame.data = frame.data;
            videoFrame.width = frame.width;
            videoFrame.height = frame.height;
            videoFrame.timestampUs = frame.timestampUs;
            videoFrame.stride = frame.stride;
            videoFrame.format = frame.format;
            videoFrame.codec = frame.codec;
            
            deliverVideoFrame(sessionId, videoFrame);
        });

    webRtcReceiver_->setAudioFrameCallback(
        [this](const std::string& sessionId, const WebRtcAudioFrame& frame) {
            // Convert WebRTC frame to SessionManager frame
            AudioFrame audioFrame;
            audioFrame.data = frame.data;
            audioFrame.sampleRate = frame.sampleRate;
            audioFrame.channels = frame.channels;
            audioFrame.timestampUs = frame.timestampUs;
            audioFrame.frames = frame.frames;
            
            deliverAudioFrame(sessionId, audioFrame);
        });

    webRtcReceiver_->setSessionStateCallback(
        [this](const std::string& sessionId, const std::string& state) {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            auto it = sessions_.find(sessionId);
            if (it != sessions_.end()) {
                it->second.state = state;
            }
        });

    webRtcReceiver_->setIceCandidateCallback(
        [this](const std::string& sessionId, const std::string& candidate) {
            // In production, this would be sent to the phone via signaling
            std::cout << "[SessionManager] ICE candidate for " << sessionId << ": " << candidate << std::endl;
        });

    initialized_ = true;
    std::cout << "[SessionManager] Initialized successfully" << std::endl;
    return true;
}

bool SessionManager::addSession(const CameraSession& session) {
    if (!initialized_) {
        if (!initialize()) {
            return false;
        }
    }

    if (session.deviceId.empty() || session.sessionId.empty()) {
        std::cerr << "[SessionManager] Invalid session: empty deviceId or sessionId" << std::endl;
        return false;
    }

    // Create WebRTC session config
    WebRtcSessionConfig config;
    config.sessionId = session.sessionId;
    config.deviceId = session.deviceId;
    config.videoEnabled = session.videoEnabled;
    config.audioEnabled = session.audioEnabled;
    config.dtlsEnabled = true;
    config.srtpEnabled = true;
    
    // Note: remoteSdp would come from signaling (phone sends offer)
    // For now, we create the session and wait for remote offer

    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        if (sessions_.find(session.sessionId) != sessions_.end()) {
            std::cerr << "[SessionManager] Session already exists: " << session.sessionId << std::endl;
            return false;
        }
        
        // Store session
        CameraSession newSession = session;
        newSession.state = "waiting_for_offer";
        sessions_[session.sessionId] = newSession;
    }

    // Create WebRTC session (will wait for remote offer)
    if (!webRtcReceiver_->createSession(config)) {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_.erase(session.sessionId);
        std::cerr << "[SessionManager] Failed to create WebRTC session" << std::endl;
        return false;
    }

    std::cout << "[SessionManager] Added session: " << session.sessionId << " for device: " << session.deviceId << std::endl;
    return true;
}

bool SessionManager::removeSession(const std::string& sessionId) {
    if (sessionId.empty()) {
        return false;
    }

    // Remove from WebRTC receiver
    if (webRtcReceiver_) {
        webRtcReceiver_->removeSession(sessionId);
    }

    // Remove from local storage
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    bool removed = sessions_.erase(sessionId) > 0;
    
    if (removed) {
        std::cout << "[SessionManager] Removed session: " << sessionId << std::endl;
    }
    
    return removed;
}

bool SessionManager::setAudioEnabled(const std::string& sessionId, bool enabled) {
    if (webRtcReceiver_) {
        webRtcReceiver_->setAudioEnabled(sessionId, enabled);
    }
    
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return false;
    }
    it->second.audioEnabled = enabled;
    return true;
}

bool SessionManager::setMuted(const std::string& sessionId, bool muted) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return false;
    }
    it->second.muted = muted;
    
    // Also apply to audio distributor via gain (0 = muted)
    // This will be handled by AudioDistributor when it gets the frame
    return true;
}

bool SessionManager::setVideoEnabled(const std::string& sessionId, bool enabled) {
    if (webRtcReceiver_) {
        webRtcReceiver_->setVideoEnabled(sessionId, enabled);
    }
    
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return false;
    }
    it->second.videoEnabled = enabled;
    return true;
}

bool SessionManager::setGain(const std::string& sessionId, float gain) {
    if (gain < 0.0f || gain > 4.0f) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return false;
    }
    it->second.gain = gain;
    return true;
}

bool SessionManager::setAudioDelay(const std::string& sessionId, int delayMs) {
    if (delayMs < 0 || delayMs > 5000) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return false;
    }
    it->second.audioDelayMs = delayMs;
    return true;
}

std::optional<CameraSession> SessionManager::getSession(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool SessionManager::hasSession(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    return sessions_.find(sessionId) != sessions_.end();
}

std::vector<std::string> SessionManager::getAllSessionIds() const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    std::vector<std::string> ids;
    ids.reserve(sessions_.size());
    for (const auto& [id, session] : sessions_) {
        ids.push_back(id);
    }
    return ids;
}

void SessionManager::setVideoFrameCallback(VideoFrameCallback callback) {
    videoFrameCallback_ = std::move(callback);
}

void SessionManager::setAudioFrameCallback(AudioFrameCallback callback) {
    audioFrameCallback_ = std::move(callback);
}

std::optional<VideoFrame> SessionManager::getVideoFrame(const std::string& sessionId) {
    if (!hasSession(sessionId)) {
        return std::nullopt;
    }
    if (videoFrameCallback_) {
        return videoFrameCallback_(sessionId);
    }
    return std::nullopt;
}

std::optional<AudioFrame> SessionManager::getAudioFrame(const std::string& sessionId) {
    if (!hasSession(sessionId)) {
        return std::nullopt;
    }
    if (audioFrameCallback_) {
        return audioFrameCallback_(sessionId);
    }
    return std::nullopt;
}

void SessionManager::registerVideoConsumer(VideoFrameConsumer consumer) {
    std::lock_guard<std::mutex> lock(consumerMutex_);
    videoConsumer_ = std::move(consumer);
}

void SessionManager::registerAudioConsumer(AudioFrameConsumer consumer) {
    std::lock_guard<std::mutex> lock(consumerMutex_);
    audioConsumer_ = std::move(consumer);
}

void SessionManager::unregisterVideoConsumer() {
    std::lock_guard<std::mutex> lock(consumerMutex_);
    videoConsumer_ = nullptr;
}

void SessionManager::unregisterAudioConsumer() {
    std::lock_guard<std::mutex> lock(consumerMutex_);
    audioConsumer_ = nullptr;
}

bool SessionManager::setRemoteAnswer(const std::string& sessionId, const std::string& remoteSdp) {
    if (!webRtcReceiver_) {
        return false;
    }
    return webRtcReceiver_->setRemoteAnswer(sessionId, remoteSdp);
}

bool SessionManager::addIceCandidate(const std::string& sessionId, const std::string& candidate) {
    if (!webRtcReceiver_) {
        return false;
    }
    return webRtcReceiver_->addIceCandidate(sessionId, candidate);
}

std::optional<std::string> SessionManager::getLocalOffer(const std::string& sessionId) {
    if (!webRtcReceiver_) {
        return std::nullopt;
    }
    return webRtcReceiver_->getLocalOffer(sessionId);
}

std::optional<std::string> SessionManager::getLocalAnswer(const std::string& sessionId) {
    if (!webRtcReceiver_) {
        return std::nullopt;
    }
    return webRtcReceiver_->getLocalAnswer(sessionId);
}

std::string SessionManager::getSessionState(const std::string& sessionId) const {
    if (!webRtcReceiver_) {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto it = sessions_.find(sessionId);
        if (it != sessions_.end()) {
            return it->second.state;
        }
        return "unknown";
    }
    return webRtcReceiver_->getSessionState(sessionId);
}

void SessionManager::shutdown() {
    if (!initialized_) {
        return;
    }

    // Shutdown WebRTC receiver
    if (webRtcReceiver_) {
        webRtcReceiver_->shutdown();
        webRtcReceiver_.reset();
    }

    // Clear sessions
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_.clear();
    }

    // Clear consumers
    {
        std::lock_guard<std::mutex> lock(consumerMutex_);
        videoConsumer_ = nullptr;
        audioConsumer_ = nullptr;
    }

    // Clear callbacks
    videoFrameCallback_ = nullptr;
    audioFrameCallback_ = nullptr;

    initialized_ = false;
    std::cout << "[SessionManager] Shutdown complete" << std::endl;
}

void SessionManager::deliverVideoFrame(const std::string& sessionId, const VideoFrame& frame) {
    // Update session metadata
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto it = sessions_.find(sessionId);
        if (it != sessions_.end()) {
            it->second.lastFrameTimestampUs = frame.timestampUs;
            if (it->second.videoWidth == 0) {
                it->second.videoWidth = frame.width;
                it->second.videoHeight = frame.height;
            }
            if (it->second.videoCodec.empty()) {
                it->second.videoCodec = frame.codec;
            }
        }
    }

    // Deliver to real-time consumer
    {
        std::lock_guard<std::mutex> lock(consumerMutex_);
        if (videoConsumer_) {
            videoConsumer_(sessionId, frame);
        }
    }

    // Also available via pull callback (legacy)
    // Note: pull callback is set by external code and called on demand
}

void SessionManager::deliverAudioFrame(const std::string& sessionId, const AudioFrame& frame) {
    // Update session metadata
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto it = sessions_.find(sessionId);
        if (it != sessions_.end()) {
            it->second.lastAudioTimestampUs = frame.timestampUs;
            if (it->second.audioCodec.empty()) {
                it->second.audioCodec = "Opus";
            }
        }
    }

    // Deliver to real-time consumer
    {
        std::lock_guard<std::mutex> lock(consumerMutex_);
        if (audioConsumer_) {
            audioConsumer_(sessionId, frame);
        }
    }
}

} // namespace phonecam