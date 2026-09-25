#include "phonecam/SignalingServer.h"

#include <iostream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <random>
#include <nlohmann/json.hpp>

namespace phonecam {

// Internal implementation using a simple TCP server with JSON-based signaling
// In production, this should be replaced with a proper WebSocket server (e.g., Boost.Beast, uWebSockets.js)

struct DeviceConnection {
    std::string deviceId;
    std::vector<std::string> sessionIds;
    std::chrono::steady_clock::time_point lastActivity;
    bool authenticated = false;
    uint32_t sequenceCounter = 0;
};

class SignalingServer::Impl {
public:
    Impl() = default;
    ~Impl() { stop(); }

    bool start(const SignalingServerConfig& config) {
        config_ = config;
        running_ = true;
        
        // For now, we'll implement a simple signaling server using a basic approach
        // In production, this should use a proper WebSocket library
        std::cout << "[SignalingServer] Starting on " << config.bindAddress << ":" << config.port << std::endl;
        std::cout << "[SignalingServer] WARNING: Using stub implementation. Replace with real WebSocket server for production." << std::endl;
        
        // Start worker thread for processing
        workerThread_ = std::thread(&Impl::workerLoop, this);
        
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        
        // Clean up connections
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            connections_.clear();
        }
        
        std::cout << "[SignalingServer] Stopped" << std::endl;
    }

    bool isRunning() const { return running_.load(); }

    // Send methods
    bool sendOffer(const std::string& sessionId, const std::string& deviceId, const std::string& sdp) {
        return sendToDevice(deviceId, createMessage(SignalingMessageType::OFFER, sessionId, deviceId, sdp));
    }

    bool sendAnswer(const std::string& sessionId, const std::string& sdp) {
        // Find device for session
        std::string deviceId = findDeviceForSession(sessionId);
        if (deviceId.empty()) {
            return false;
        }
        return sendToDevice(deviceId, createMessage(SignalingMessageType::ANSWER, sessionId, deviceId, sdp));
    }

    bool sendIceCandidate(const std::string& sessionId, const std::string& candidate) {
        std::string deviceId = findDeviceForSession(sessionId);
        if (deviceId.empty()) {
            return false;
        }
        return sendToDevice(deviceId, createMessage(SignalingMessageType::ICE_CANDIDATE, sessionId, deviceId, candidate));
    }

    bool sendSessionClose(const std::string& sessionId) {
        std::string deviceId = findDeviceForSession(sessionId);
        if (deviceId.empty()) {
            return false;
        }
        return sendToDevice(deviceId, createMessage(SignalingMessageType::SESSION_CLOSE, sessionId, deviceId, ""));
    }

    bool sendError(const std::string& sessionId, const std::string& error) {
        std::string deviceId = findDeviceForSession(sessionId);
        if (deviceId.empty()) {
            return false;
        }
        nlohmann::json errorJson;
        errorJson["error"] = error;
        return sendToDevice(deviceId, createMessage(SignalingMessageType::SIGNAL_ERROR, sessionId, deviceId, errorJson.dump()));
    }

    void broadcast(const SignalingMessage& /*message*/) {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (auto& [connId, conn] : connections_) {
            // In real implementation, send to each connection
            (void)conn;
        }
    }

    size_t getConnectedDeviceCount() const {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        return connections_.size();
    }

    std::vector<std::string> getSessionsForDevice(const std::string& deviceId) const {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = connections_.find(deviceId);
        if (it != connections_.end()) {
            return it->second.sessionIds;
        }
        return {};
    }

    void processEvents() {
        // Process any pending operations
        // In real implementation, this would handle WebSocket events
        cleanupStaleConnections();
    }

    // Callbacks
    OnOfferCallback onOfferCallback_;
    OnAnswerCallback onAnswerCallback_;
    OnIceCandidateCallback onIceCandidateCallback_;
    OnSessionInitCallback onSessionInitCallback_;
    OnSessionCloseCallback onSessionCloseCallback_;
    OnErrorCallback onErrorCallback_;

private:
    SignalingServerConfig config_;
    std::atomic<bool> running_{false};
    std::thread workerThread_;
    mutable std::mutex connectionsMutex_;
    std::unordered_map<std::string, DeviceConnection> connections_;
    uint64_t messageCounter_ = 0;

    // Message creation
    SignalingMessage createMessage(SignalingMessageType type, const std::string& sessionId, 
                                   const std::string& deviceId, const std::string& payload) {
        SignalingMessage msg;
        msg.type = type;
        msg.sessionId = sessionId;
        msg.deviceId = deviceId;
        msg.payload = payload;
        msg.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        msg.sequence = static_cast<uint32_t>(++messageCounter_);
        return msg;
    }

    bool sendToDevice(const std::string& deviceId, const SignalingMessage& message) {
        // In real implementation, this would send over WebSocket
        // For now, just log
        std::cout << "[SignalingServer] Send to " << deviceId << ": type=" << static_cast<int>(message.type)
                  << " session=" << message.sessionId << std::endl;
        return true;
    }

    std::string findDeviceForSession(const std::string& sessionId) const {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (const auto& [deviceId, conn] : connections_) {
            if (std::find(conn.sessionIds.begin(), conn.sessionIds.end(), sessionId) != conn.sessionIds.end()) {
                return deviceId;
            }
        }
        return "";
    }

    void workerLoop() {
        while (running_.load()) {
            processEvents();
            
            if (config_.enablePingPong) {
                sendPings();
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void sendPings() {
        // Send ping to all connections
        // In real implementation, send WebSocket ping frame
    }

    void cleanupStaleConnections() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        
        auto it = connections_.begin();
        while (it != connections_.end()) {
            if (now - it->second.lastActivity > config_.connectionTimeout) {
                std::cout << "[SignalingServer] Removing stale connection: " << it->first << std::endl;
                if (onSessionCloseCallback_) {
                    for (const auto& sessionId : it->second.sessionIds) {
                        onSessionCloseCallback_(sessionId);
                    }
                }
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Message handling (would be called from WebSocket callbacks in real implementation)
    void handleMessage(const std::string& deviceId, const std::string& jsonMessage) {
        try {
            auto json = nlohmann::json::parse(jsonMessage);
            
            std::string typeStr = json.value("type", "");
            std::string sessionId = json.value("sessionId", "");
            std::string payload = json.value("payload", "");
            
            SignalingMessageType type;
            if (typeStr == "offer") type = SignalingMessageType::OFFER;
            else if (typeStr == "answer") type = SignalingMessageType::ANSWER;
            else if (typeStr == "iceCandidate") type = SignalingMessageType::ICE_CANDIDATE;
            else if (typeStr == "sessionInit") type = SignalingMessageType::SESSION_INIT;
            else if (typeStr == "sessionClose") type = SignalingMessageType::SESSION_CLOSE;
            else if (typeStr == "error") type = SignalingMessageType::SIGNAL_ERROR;
            else {
                if (onErrorCallback_) onErrorCallback_(sessionId, "Unknown message type: " + typeStr);
                return;
            }
            
            // Update connection activity
            {
                std::lock_guard<std::mutex> lock(connectionsMutex_);
                auto& conn = connections_[deviceId];
                conn.deviceId = deviceId;
                conn.lastActivity = std::chrono::steady_clock::now();
            }
            
            switch (type) {
                case SignalingMessageType::OFFER:
                    if (onOfferCallback_) onOfferCallback_(sessionId, deviceId, payload);
                    break;
                case SignalingMessageType::ANSWER:
                    if (onAnswerCallback_) onAnswerCallback_(sessionId, payload);
                    break;
                case SignalingMessageType::ICE_CANDIDATE:
                    if (onIceCandidateCallback_) onIceCandidateCallback_(sessionId, payload);
                    break;
                case SignalingMessageType::SESSION_INIT: {
                    std::lock_guard<std::mutex> lock(connectionsMutex_);
                    auto& conn = connections_[deviceId];
                    conn.deviceId = deviceId;
                    conn.lastActivity = std::chrono::steady_clock::now();
                    if (std::find(conn.sessionIds.begin(), conn.sessionIds.end(), sessionId) == conn.sessionIds.end()) {
                        conn.sessionIds.push_back(sessionId);
                    }
                    if (onSessionInitCallback_) onSessionInitCallback_(sessionId, deviceId);
                    break;
                }
                case SignalingMessageType::SESSION_CLOSE: {
                    std::lock_guard<std::mutex> lock(connectionsMutex_);
                    auto connIt = connections_.find(deviceId);
                    if (connIt != connections_.end()) {
                        auto& sessionIds = connIt->second.sessionIds;
                        sessionIds.erase(std::remove(sessionIds.begin(), sessionIds.end(), sessionId), sessionIds.end());
                        if (sessionIds.empty()) {
                            connections_.erase(connIt);
                        }
                    }
                    if (onSessionCloseCallback_) onSessionCloseCallback_(sessionId);
                    break;
                }
                default:
                    break;
            }
        } catch (const std::exception& e) {
            std::cerr << "[SignalingServer] Error handling message: " << e.what() << std::endl;
            if (onErrorCallback_) onErrorCallback_("", "Parse error: " + std::string(e.what()));
        }
    }
};

SignalingServer::SignalingServer() : impl_(std::make_unique<Impl>()) {}
SignalingServer::~SignalingServer() = default;

bool SignalingServer::start(const SignalingServerConfig& config) {
    return impl_->start(config);
}

void SignalingServer::stop() {
    impl_->stop();
}

bool SignalingServer::sendOffer(const std::string& sessionId, const std::string& deviceId, const std::string& sdp) {
    return impl_->sendOffer(sessionId, deviceId, sdp);
}

bool SignalingServer::sendAnswer(const std::string& sessionId, const std::string& sdp) {
    return impl_->sendAnswer(sessionId, sdp);
}

bool SignalingServer::sendIceCandidate(const std::string& sessionId, const std::string& candidate) {
    return impl_->sendIceCandidate(sessionId, candidate);
}

bool SignalingServer::sendSessionClose(const std::string& sessionId) {
    return impl_->sendSessionClose(sessionId);
}

bool SignalingServer::sendError(const std::string& sessionId, const std::string& error) {
    return impl_->sendError(sessionId, error);
}

void SignalingServer::broadcast(const SignalingMessage& message) {
    impl_->broadcast(message);
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

} // namespace phonecam