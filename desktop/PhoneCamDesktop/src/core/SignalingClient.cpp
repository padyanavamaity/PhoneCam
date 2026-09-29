#include "phonecam/SignalingClient.h"

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <queue>
#include <functional>

namespace phonecam {

// Helper to convert string to SignalingMessageType
static SignalingMessageType stringToMessageType(const std::string& typeStr) {
    if (typeStr == "hello") return SignalingMessageType::HELLO;
    if (typeStr == "offer") return SignalingMessageType::OFFER;
    if (typeStr == "answer") return SignalingMessageType::ANSWER;
    if (typeStr == "candidate") return SignalingMessageType::CANDIDATE;
    if (typeStr == "command") return SignalingMessageType::COMMAND;
    if (typeStr == "ping") return SignalingMessageType::PING;
    if (typeStr == "pong") return SignalingMessageType::PONG;
    if (typeStr == "bye") return SignalingMessageType::BYE;
    if (typeStr == "error") return SignalingMessageType::ERROR_MSG;
    if (typeStr == "status") return SignalingMessageType::STATUS;
    if (typeStr == "disconnected") return SignalingMessageType::DISCONNECTED;
    return SignalingMessageType::UNKNOWN;
}

// Helper to parse HelloMessage from JSON
static std::optional<HelloMessage> parseHelloMessage(const nlohmann::json& payload) {
    HelloMessage msg;
    try {
        if (payload.contains("protocol") && payload["protocol"].is_string())
            msg.protocol = payload["protocol"].get<std::string>();
        if (payload.contains("sessionId") && payload["sessionId"].is_string())
            msg.sessionId = payload["sessionId"].get<std::string>();
        if (payload.contains("deviceId") && payload["deviceId"].is_string())
            msg.deviceId = payload["deviceId"].get<std::string>();
        if (payload.contains("name") && payload["name"].is_string())
            msg.name = payload["name"].get<std::string>();
        if (payload.contains("state") && payload["state"].is_string())
            msg.state = payload["state"].get<std::string>();
        if (payload.contains("camera") && payload["camera"].is_string())
            msg.camera = payload["camera"].get<std::string>();
        if (payload.contains("micEnabled") && payload["micEnabled"].is_boolean())
            msg.micEnabled = payload["micEnabled"].get<bool>();
        if (payload.contains("bitrateKbps") && payload["bitrateKbps"].is_number())
            msg.bitrateKbps = payload["bitrateKbps"].get<int>();
        if (payload.contains("width") && payload["width"].is_number())
            msg.width = payload["width"].get<int>();
        if (payload.contains("height") && payload["height"].is_number())
            msg.height = payload["height"].get<int>();
        if (payload.contains("fps") && payload["fps"].is_number())
            msg.fps = payload["fps"].get<int>();
        return msg;
    } catch (const std::exception& e) {
        std::cerr << "[SignalingClient] Failed to parse hello message: " << e.what() << std::endl;
        return std::nullopt;
    }
}

// Helper to parse OfferMessage from JSON
static std::optional<OfferMessage> parseOfferMessage(const nlohmann::json& payload) {
    OfferMessage msg;
    try {
        if (payload.contains("sessionId") && payload["sessionId"].is_string())
            msg.sessionId = payload["sessionId"].get<std::string>();
        if (payload.contains("sdp") && payload["sdp"].is_string())
            msg.sdp = payload["sdp"].get<std::string>();
        else
            return std::nullopt; // SDP is required
        return msg;
    } catch (const std::exception& e) {
        std::cerr << "[SignalingClient] Failed to parse offer message: " << e.what() << std::endl;
        return std::nullopt;
    }
}

// Helper to parse AnswerMessage from JSON
static std::optional<AnswerMessage> parseAnswerMessage(const nlohmann::json& payload) {
    AnswerMessage msg;
    try {
        if (payload.contains("sessionId") && payload["sessionId"].is_string())
            msg.sessionId = payload["sessionId"].get<std::string>();
        if (payload.contains("sdp") && payload["sdp"].is_string())
            msg.sdp = payload["sdp"].get<std::string>();
        else
            return std::nullopt;
        return msg;
    } catch (const std::exception& e) {
        std::cerr << "[SignalingClient] Failed to parse answer message: " << e.what() << std::endl;
        return std::nullopt;
    }
}

// Helper to parse CandidateMessage from JSON
static std::optional<CandidateMessage> parseCandidateMessage(const nlohmann::json& payload) {
    CandidateMessage msg;
    try {
        if (payload.contains("sessionId") && payload["sessionId"].is_string())
            msg.sessionId = payload["sessionId"].get<std::string>();
        if (payload.contains("sdpMid") && payload["sdpMid"].is_string())
            msg.sdpMid = payload["sdpMid"].get<std::string>();
        if (payload.contains("sdpMLineIndex") && payload["sdpMLineIndex"].is_number())
            msg.sdpMLineIndex = payload["sdpMLineIndex"].get<int>();
        if (payload.contains("candidate") && payload["candidate"].is_string())
            msg.candidate = payload["candidate"].get<std::string>();
        else
            return std::nullopt; // candidate is required
        return msg;
    } catch (const std::exception& e) {
        std::cerr << "[SignalingClient] Failed to parse candidate message: " << e.what() << std::endl;
        return std::nullopt;
    }
}

// Helper to parse CommandMessage from JSON
static std::optional<CommandMessage> parseCommandMessage(const nlohmann::json& payload) {
    CommandMessage msg;
    try {
        if (payload.contains("sessionId") && payload["sessionId"].is_string())
            msg.sessionId = payload["sessionId"].get<std::string>();
        if (payload.contains("action") && payload["action"].is_string())
            msg.action = payload["action"].get<std::string>();
        else
            return std::nullopt; // action is required

        // Parse action-specific fields
        if (payload.contains("micEnabled") && payload["micEnabled"].is_boolean())
            msg.micEnabled = payload["micEnabled"].get<bool>();
        if (payload.contains("camera") && payload["camera"].is_string())
            msg.camera = payload["camera"].get<std::string>();
        if (payload.contains("bitrateKbps") && payload["bitrateKbps"].is_number())
            msg.bitrateKbps = payload["bitrateKbps"].get<int>();
        if (payload.contains("captureMode") && payload["captureMode"].is_string())
            msg.captureMode = payload["captureMode"].get<std::string>();
        if (payload.contains("newName") && payload["newName"].is_string())
            msg.newName = payload["newName"].get<std::string>();
        return msg;
    } catch (const std::exception& e) {
        std::cerr << "[SignalingClient] Failed to parse command message: " << e.what() << std::endl;
        return std::nullopt;
    }
}

// Helper to parse ErrorMessage from JSON
static std::optional<ErrorMessage> parseErrorMessage(const nlohmann::json& payload) {
    ErrorMessage msg;
    try {
        if (payload.contains("sessionId") && payload["sessionId"].is_string())
            msg.sessionId = payload["sessionId"].get<std::string>();
        if (payload.contains("code") && payload["code"].is_string())
            msg.code = payload["code"].get<std::string>();
        if (payload.contains("message") && payload["message"].is_string())
            msg.message = payload["message"].get<std::string>();
        return msg;
    } catch (const std::exception& e) {
        std::cerr << "[SignalingClient] Failed to parse error message: " << e.what() << std::endl;
        return std::nullopt;
    }
}

// Helper to parse DisconnectMessage from JSON
static std::optional<DisconnectMessage> parseDisconnectMessage(const nlohmann::json& payload) {
    DisconnectMessage msg;
    try {
        if (payload.contains("sessionId") && payload["sessionId"].is_string())
            msg.sessionId = payload["sessionId"].get<std::string>();
        if (payload.contains("reason") && payload["reason"].is_string())
            msg.reason = payload["reason"].get<std::string>();
        return msg;
    } catch (const std::exception& e) {
        std::cerr << "[SignalingClient] Failed to parse disconnect message: " << e.what() << std::endl;
        return std::nullopt;
    }
}

// Helper to parse StatusMessage from JSON
static std::optional<StatusMessage> parseStatusMessage(const nlohmann::json& payload) {
    StatusMessage msg;
    try {
        if (payload.contains("sessionId") && payload["sessionId"].is_string())
            msg.sessionId = payload["sessionId"].get<std::string>();
        if (payload.contains("state") && payload["state"].is_string())
            msg.state = payload["state"].get<std::string>();
        if (payload.contains("micEnabled") && payload["micEnabled"].is_boolean())
            msg.micEnabled = payload["micEnabled"].get<bool>();
        if (payload.contains("camera") && payload["camera"].is_string())
            msg.camera = payload["camera"].get<std::string>();
        if (payload.contains("bitrateKbps") && payload["bitrateKbps"].is_number())
            msg.bitrateKbps = payload["bitrateKbps"].get<int>();
        if (payload.contains("width") && payload["width"].is_number())
            msg.width = payload["width"].get<int>();
        if (payload.contains("height") && payload["height"].is_number())
            msg.height = payload["height"].get<int>();
        if (payload.contains("fps") && payload["fps"].is_number())
            msg.fps = payload["fps"].get<int>();
        return msg;
    } catch (const std::exception& e) {
        std::cerr << "[SignalingClient] Failed to parse status message: " << e.what() << std::endl;
        return std::nullopt;
    }
}

// Internal implementation
class SignalingClient::Impl {
public:
    Impl(SignalingClient* owner) : owner_(owner) {}
    ~Impl() {
        stop();
    }

    bool connect(const SignalingClientConfig& config) {
        config_ = config;
        
        // Initialize IXWebSocket
        ix::initNetSystem();
        
        // Build WebSocket URL
        std::string url = "ws://" + config_.host + ":" + std::to_string(config_.port) + config_.path;
        
        webSocket_ = std::make_unique<ix::WebSocket>();
        webSocket_->setUrl(url);
        
        // Configure timeouts
        webSocket_->setPingInterval(static_cast<int>(config_.pingInterval.count() * 1000));
        
        // Set up message callback - handles all message types including Open, Close, Error
        webSocket_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            onWebSocketMessage(msg);
        });
        
        // Start the connection
        state_ = ConnectionState::CONNECTING;
        owner_->notifyConnectionStateChange(ConnectionState::CONNECTING);
        
        webSocket_->start();
        
        // Wait for connection with timeout
        auto startTime = std::chrono::steady_clock::now();
        while (state_ == ConnectionState::CONNECTING) {
            if (std::chrono::steady_clock::now() - startTime > config_.connectTimeout) {
                std::cerr << "[SignalingClient] Connection timeout" << std::endl;
                webSocket_->stop();
                state_ = ConnectionState::ERROR_STATE;
                owner_->notifyConnectionStateChange(ConnectionState::ERROR_STATE, "Connection timeout");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        
        if (state_ == ConnectionState::CONNECTED) {
            // Start ping thread
            pingThreadRunning_ = true;
            pingThread_ = std::thread(&Impl::pingLoop, this);
            
            // Start message processing thread
            processThreadRunning_ = true;
            processThread_ = std::thread(&Impl::processLoop, this);
            
            return true;
        }
        
        return false;
    }

    void disconnect() {
        stop();
    }

    bool sendMessage(const nlohmann::json& message) {
        if (state_ != ConnectionState::CONNECTED || !webSocket_) {
            return false;
        }
        
        std::string jsonStr = message.dump();
        if (jsonStr.size() > config_.maxMessageSize) {
            std::cerr << "[SignalingClient] Message too large: " << jsonStr.size() << " bytes" << std::endl;
            return false;
        }
        
        webSocket_->send(jsonStr);
        return true;
    }

    bool sendAnswer(const std::string& sessionId, const std::string& sdp) {
        nlohmann::json msg;
        msg["type"] = "answer";
        msg["sessionId"] = sessionId;
        msg["sdp"] = sdp;
        bool result = sendMessage(msg);
        if (result) {
            std::cout << "[SignalingClient] Answer sent: sessionId=" << sessionId
                      << " sdpBytes=" << sdp.size() << std::endl;
        } else {
            std::cerr << "[SignalingClient] Failed to send answer: sessionId=" << sessionId << std::endl;
        }
        return result;
    }

    bool sendCandidate(const std::string& sessionId, const std::string& sdpMid, int sdpMLineIndex, const std::string& candidate) {
        nlohmann::json msg;
        msg["type"] = "candidate";
        msg["sessionId"] = sessionId;
        msg["sdpMid"] = sdpMid;
        msg["sdpMLineIndex"] = sdpMLineIndex;
        msg["candidate"] = candidate;
        bool result = sendMessage(msg);
        if (result) {
            std::cout << "[SignalingClient] ICE candidate sent: sessionId=" << sessionId
                      << " mid=" << sdpMid << " mline=" << sdpMLineIndex
                      << " candidateBytes=" << candidate.size() << std::endl;
        } else {
            std::cerr << "[SignalingClient] Failed to send ICE candidate: sessionId=" << sessionId << std::endl;
        }
        return result;
    }

    bool sendCommand(const std::string& sessionId, const std::string& action, const nlohmann::json& params = {}) {
        nlohmann::json msg;
        msg["type"] = "command";
        msg["sessionId"] = sessionId;
        msg["action"] = action;
        if (!params.empty()) {
            for (auto& [key, value] : params.items()) {
                msg[key] = value;
            }
        }
        return sendMessage(msg);
    }

    bool sendPing() {
        nlohmann::json msg;
        msg["type"] = "ping";
        return sendMessage(msg);
    }

    bool sendBye(const std::string& sessionId) {
        nlohmann::json msg;
        msg["type"] = "bye";
        msg["sessionId"] = sessionId;
        return sendMessage(msg);
    }

    void processEvents() {
        // Process any queued messages
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            callbacks.swap(pendingCallbacks_);
        }
        
        for (auto& cb : callbacks) {
            cb();
        }
    }

    ConnectionState getState() const {
        return state_;
    }

private:
    void stop() {
        // Stop threads
        pingThreadRunning_ = false;
        if (pingThread_.joinable()) {
            pingThread_.join();
        }
        
        processThreadRunning_ = false;
        processCV_.notify_all();
        if (processThread_.joinable()) {
            processThread_.join();
        }
        
        // Stop WebSocket
        if (webSocket_) {
            webSocket_->stop();
            webSocket_.reset();
        }
        
        // Cleanup
        ix::uninitNetSystem();
        
        state_ = ConnectionState::DISCONNECTED;
        owner_->notifyConnectionStateChange(ConnectionState::DISCONNECTED);
    }

    void onWebSocketMessage(const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message) {
            // Queue message for processing
            {
                std::lock_guard<std::mutex> lock(processMutex_);
                messageQueue_.push(msg->str);
            }
            processCV_.notify_one();
        } else if (msg->type == ix::WebSocketMessageType::Open) {
            std::cout << "[SignalingClient] WebSocket connected to " << config_.host << ":" << config_.port << config_.path << std::endl;
            state_ = ConnectionState::CONNECTED;
            lastPongTime_ = std::chrono::steady_clock::now();
            owner_->notifyConnectionStateChange(ConnectionState::CONNECTED);
        } else if (msg->type == ix::WebSocketMessageType::Close) {
            std::cerr << "[SignalingClient] WebSocket closed: " << msg->closeInfo.reason << std::endl;
            handleDisconnect(msg->closeInfo.reason);
        } else if (msg->type == ix::WebSocketMessageType::Error) {
            std::cerr << "[SignalingClient] WebSocket error: " << msg->errorInfo.reason << std::endl;
            handleError(msg->errorInfo.reason);
        } else if (msg->type == ix::WebSocketMessageType::Pong) {
            lastPongTime_ = std::chrono::steady_clock::now();
        }
    }

    void handleDisconnect(const std::string& reason) {
        if (state_ == ConnectionState::CONNECTED || state_ == ConnectionState::CONNECTING) {
            state_ = ConnectionState::DISCONNECTED;
            owner_->notifyConnectionStateChange(ConnectionState::DISCONNECTED, reason);
            
            // Trigger reconnect if enabled
            if (config_.autoReconnect) {
                state_ = ConnectionState::RECONNECTING;
                owner_->notifyConnectionStateChange(ConnectionState::RECONNECTING, reason);
                reconnectThread_ = std::thread(&Impl::reconnectLoop, this);
            }
        }
    }

    void handleError(const std::string& error) {
        if (state_ == ConnectionState::CONNECTED || state_ == ConnectionState::CONNECTING) {
            state_ = ConnectionState::ERROR_STATE;
            owner_->notifyConnectionStateChange(ConnectionState::ERROR_STATE, error);
            
            // Trigger reconnect if enabled
            if (config_.autoReconnect) {
                state_ = ConnectionState::RECONNECTING;
                owner_->notifyConnectionStateChange(ConnectionState::RECONNECTING, error);
                reconnectThread_ = std::thread(&Impl::reconnectLoop, this);
            }
        }
    }

    void reconnectLoop() {
        while (config_.autoReconnect && (state_ == ConnectionState::RECONNECTING || state_ == ConnectionState::DISCONNECTED || state_ == ConnectionState::ERROR_STATE)) {
            std::cout << "[SignalingClient] Reconnecting in " << config_.reconnectDelay.count() << " seconds..." << std::endl;
            std::this_thread::sleep_for(config_.reconnectDelay);
            
            if (!config_.autoReconnect) break;
            
            // Try to reconnect
            std::string url = "ws://" + config_.host + ":" + std::to_string(config_.port) + config_.path;
            
            webSocket_ = std::make_unique<ix::WebSocket>();
            webSocket_->setUrl(url);
            webSocket_->setPingInterval(static_cast<int>(config_.pingInterval.count() * 1000));
            
            webSocket_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
                onWebSocketMessage(msg);
            });
            
            webSocket_->start();
            
            // Wait for connection
            auto startTime = std::chrono::steady_clock::now();
            while (state_ == ConnectionState::RECONNECTING) {
                if (std::chrono::steady_clock::now() - startTime > config_.connectTimeout) {
                    std::cerr << "[SignalingClient] Reconnection timeout" << std::endl;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            
            if (state_ == ConnectionState::CONNECTED) {
                // Restart ping and process threads
                pingThreadRunning_ = true;
                pingThread_ = std::thread(&Impl::pingLoop, this);
                
                processThreadRunning_ = true;
                processThread_ = std::thread(&Impl::processLoop, this);
                break;
            }
        }
    }

    void pingLoop() {
        while (pingThreadRunning_ && state_ == ConnectionState::CONNECTED) {
            std::this_thread::sleep_for(config_.pingInterval);
            
            if (!pingThreadRunning_ || state_ != ConnectionState::CONNECTED) break;
            
            // Check if we received a pong recently
            auto now = std::chrono::steady_clock::now();
            auto timeSincePong = std::chrono::duration_cast<std::chrono::seconds>(now - lastPongTime_);
            
            if (timeSincePong > config_.pongTimeout) {
                std::cerr << "[SignalingClient] Pong timeout, disconnecting" << std::endl;
                handleError("Pong timeout");
                break;
            }
            
            // Send ping
            sendPing();
        }
    }

    void processLoop() {
        while (processThreadRunning_) {
            std::string messageStr;
            {
                std::unique_lock<std::mutex> lock(processMutex_);
                processCV_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return !messageQueue_.empty() || !processThreadRunning_;
                });
                
                if (!messageQueue_.empty()) {
                    messageStr = std::move(messageQueue_.front());
                    messageQueue_.pop();
                }
            }
            
            if (!messageStr.empty()) {
                processMessage(messageStr);
            }
        }
    }

    void processMessage(const std::string& messageStr) {
        try {
            nlohmann::json json = nlohmann::json::parse(messageStr);
            
            if (!json.contains("type") || !json["type"].is_string()) {
                std::cerr << "[SignalingClient] Invalid message: missing type field" << std::endl;
                return;
            }
            
            std::string typeStr = json["type"].get<std::string>();
            SignalingMessageType msgType = stringToMessageType(typeStr);
            
            nlohmann::json payload = json;
            
            // Queue callback for execution
            {
                std::lock_guard<std::mutex> lock(callbackMutex_);
                
                switch (msgType) {
                    case SignalingMessageType::HELLO: {
                        if (auto hello = parseHelloMessage(payload)) {
                            std::cout << "[SignalingClient] Hello received: deviceId=" << hello->deviceId
                                      << " sessionId=" << hello->sessionId
                                      << " name=" << hello->name
                                      << " state=" << hello->state
                                      << " camera=" << hello->camera
                                      << " micEnabled=" << (hello->micEnabled ? "true" : "false")
                                      << " bitrateKbps=" << hello->bitrateKbps
                                      << " resolution=" << hello->width << "x" << hello->height
                                      << " fps=" << hello->fps << std::endl;
                            pendingCallbacks_.push_back([this, hello]() {
                                if (owner_->onHelloCallback_) owner_->onHelloCallback_(*hello);
                            });
                        }
                        break;
                    }
                    case SignalingMessageType::OFFER: {
                        if (auto offer = parseOfferMessage(payload)) {
                            std::cout << "[SignalingClient] Offer received: sessionId=" << offer->sessionId
                                      << " sdpBytes=" << offer->sdp.size() << std::endl;
                            pendingCallbacks_.push_back([this, offer]() {
                                if (owner_->onOfferCallback_) owner_->onOfferCallback_(*offer);
                            });
                        }
                        break;
                    }
                    case SignalingMessageType::ANSWER: {
                        if (auto answer = parseAnswerMessage(payload)) {
                            pendingCallbacks_.push_back([this, answer]() {
                                if (owner_->onAnswerCallback_) owner_->onAnswerCallback_(*answer);
                            });
                        }
                        break;
                    }
                    case SignalingMessageType::CANDIDATE: {
                        if (auto candidate = parseCandidateMessage(payload)) {
                            std::cout << "[SignalingClient] ICE candidate received: sessionId=" << candidate->sessionId
                                      << " mid=" << candidate->sdpMid
                                      << " mline=" << candidate->sdpMLineIndex
                                      << " candidateBytes=" << candidate->candidate.size() << std::endl;
                            pendingCallbacks_.push_back([this, candidate]() {
                                if (owner_->onCandidateCallback_) owner_->onCandidateCallback_(*candidate);
                            });
                        }
                        break;
                    }
                    case SignalingMessageType::COMMAND: {
                        if (auto command = parseCommandMessage(payload)) {
                            pendingCallbacks_.push_back([this, command]() {
                                if (owner_->onCommandCallback_) owner_->onCommandCallback_(*command);
                            });
                        }
                        break;
                    }
                    case SignalingMessageType::ERROR_MSG: {
                        if (auto error = parseErrorMessage(payload)) {
                            std::cerr << "[SignalingClient] Error received: sessionId=" << error->sessionId
                                      << " code=" << error->code
                                      << " message=" << error->message << std::endl;
                            pendingCallbacks_.push_back([this, error]() {
                                if (owner_->onErrorCallback_) owner_->onErrorCallback_(*error);
                            });
                        }
                        break;
                    }
                    case SignalingMessageType::BYE:
                    case SignalingMessageType::DISCONNECTED: {
                        if (auto disconnect = parseDisconnectMessage(payload)) {
                            std::cout << "[SignalingClient] Disconnect received: sessionId=" << disconnect->sessionId
                                      << " reason=" << disconnect->reason << std::endl;
                            pendingCallbacks_.push_back([this, disconnect]() {
                                if (owner_->onDisconnectedCallback_) owner_->onDisconnectedCallback_(*disconnect);
                            });
                        }
                        break;
                    }
                    case SignalingMessageType::STATUS: {
                        if (auto status = parseStatusMessage(payload)) {
                            pendingCallbacks_.push_back([this, status]() {
                                if (owner_->onStatusCallback_) owner_->onStatusCallback_(*status);
                            });
                        }
                        break;
                    }
                    case SignalingMessageType::PONG: {
                        lastPongTime_ = std::chrono::steady_clock::now();
                        break;
                    }
                    default: {
                        std::cerr << "[SignalingClient] Unknown message type: " << typeStr << std::endl;
                        break;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[SignalingClient] Failed to parse message: " << e.what() << std::endl;
        }
    }

    SignalingClient* owner_;
    SignalingClientConfig config_;
    
    std::unique_ptr<ix::WebSocket> webSocket_;
    std::atomic<ConnectionState> state_{ConnectionState::DISCONNECTED};
    
    // Ping thread
    std::thread pingThread_;
    std::atomic<bool> pingThreadRunning_{false};
    std::chrono::steady_clock::time_point lastPongTime_;
    
    // Process thread
    std::thread processThread_;
    std::atomic<bool> processThreadRunning_{false};
    std::mutex processMutex_;
    std::condition_variable processCV_;
    std::queue<std::string> messageQueue_;
    
    // Reconnect thread
    std::thread reconnectThread_;
    
    // Callback queue
    std::mutex callbackMutex_;
    std::vector<std::function<void()>> pendingCallbacks_;
};

SignalingClient::SignalingClient() : impl_(std::make_unique<Impl>(this)) {}

SignalingClient::~SignalingClient() {
    disconnect();
}

bool SignalingClient::connect(const SignalingClientConfig& config) {
    return impl_->connect(config);
}

void SignalingClient::disconnect() {
    impl_->disconnect();
}

bool SignalingClient::sendAnswer(const std::string& sessionId, const std::string& sdp) {
    return impl_->sendAnswer(sessionId, sdp);
}

bool SignalingClient::sendCandidate(const std::string& sessionId, const std::string& sdpMid, int sdpMLineIndex, const std::string& candidate) {
    return impl_->sendCandidate(sessionId, sdpMid, sdpMLineIndex, candidate);
}

bool SignalingClient::sendCommand(const std::string& sessionId, const std::string& action, const nlohmann::json& params) {
    return impl_->sendCommand(sessionId, action, params);
}

bool SignalingClient::sendPing() {
    return impl_->sendPing();
}

bool SignalingClient::sendBye(const std::string& sessionId) {
    return impl_->sendBye(sessionId);
}

bool SignalingClient::sendSetMicCommand(const std::string& sessionId, bool enabled) {
    nlohmann::json params;
    params["micEnabled"] = enabled;
    return sendCommand(sessionId, "setMic", params);
}

bool SignalingClient::sendSetCameraCommand(const std::string& sessionId, const std::string& camera) {
    nlohmann::json params;
    params["camera"] = camera;
    return sendCommand(sessionId, "setCamera", params);
}

bool SignalingClient::sendSetBitrateCommand(const std::string& sessionId, int bitrateKbps) {
    nlohmann::json params;
    params["bitrateKbps"] = bitrateKbps;
    return sendCommand(sessionId, "setBitrate", params);
}

bool SignalingClient::sendSetCaptureModeCommand(const std::string& sessionId, const std::string& mode) {
    nlohmann::json params;
    params["captureMode"] = mode;
    return sendCommand(sessionId, "setCaptureMode", params);
}

bool SignalingClient::sendRenameCommand(const std::string& sessionId, const std::string& newName) {
    nlohmann::json params;
    params["newName"] = newName;
    return sendCommand(sessionId, "rename", params);
}

bool SignalingClient::sendDisconnectCommand(const std::string& sessionId) {
    return sendCommand(sessionId, "disconnect", {});
}

bool SignalingClient::sendGetStatusCommand(const std::string& sessionId) {
    return sendCommand(sessionId, "getStatus", {});
}

void SignalingClient::processEvents() {
    impl_->processEvents();
}

void SignalingClient::notifyConnectionStateChange(ConnectionState newState, const std::string& error) {
    if (onConnectionStateChangeCallback_) {
        onConnectionStateChangeCallback_(newState, error);
    }
}

} // namespace phonecam