#include "phonecam/SecurityManager.h"
#include "phonecam/SecureStorage.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>
#include <nlohmann/json.hpp>

namespace phonecam {

// Static SecureStorage instance for master key persistence
static SecureStorage g_secureStorage;

// Static member initialization
const std::vector<CommandType> SecurityManager::allowedCommands_ = {
    CommandType::PAIR_REQUEST,
    CommandType::PAIR_RESPONSE,
    CommandType::SESSION_CREATE,
    CommandType::SESSION_DESTROY,
    CommandType::SESSION_RECONNECT,
    CommandType::VIDEO_START,
    CommandType::VIDEO_STOP,
    CommandType::VIDEO_CONFIG,
    CommandType::CAMERA_SWITCH,
    CommandType::TORCH_TOGGLE,
    CommandType::ZOOM_SET,
    CommandType::FOCUS_SET,
    CommandType::EXPOSURE_SET,
    CommandType::AUDIO_START,
    CommandType::AUDIO_STOP,
    CommandType::AUDIO_MUTE,
    CommandType::AUDIO_UNMUTE,
    CommandType::AUDIO_GAIN,
    CommandType::AUDIO_DELAY,
    CommandType::AUDIO_CONFIG,
    CommandType::PING,
    CommandType::PONG,
    CommandType::STATS_REQUEST,
    CommandType::STATS_RESPONSE,
    CommandType::KEY_EXCHANGE,
    CommandType::AUTH_CHALLENGE,
    CommandType::AUTH_RESPONSE,
    CommandType::TOKEN_REFRESH
};

SecurityManager::SecurityManager() {
    // Load master key from secure storage (Windows DPAPI)
    // If not found (first run), generate a new one and persist it
    auto storedKey = g_secureStorage.retrieveMasterKey();
    if (storedKey && storedKey->size() == 32) {
        masterKey_ = *storedKey;
    } else {
        // First run or key corrupted - generate new master key
        masterKey_ = generateRandomBytes(32);
        // Persist the new master key
        g_secureStorage.storeMasterKey(masterKey_);
    }
}

SecurityManager::~SecurityManager() {
    // Ensure master key is persisted on shutdown
    // (In practice, it's already persisted on generation, but this ensures
    // any future key rotation would be saved)
    if (!masterKey_.empty()) {
        g_secureStorage.storeMasterKey(masterKey_);
    }
}

SecurityManager::SecurityManager(SecurityManager&& other) noexcept
    : sessionTokens_(std::move(other.sessionTokens_))
    , deviceSessions_(std::move(other.deviceSessions_))
    , messageSequences_(std::move(other.messageSequences_))
    , sessionNonces_(std::move(other.sessionNonces_))
    , rateLimits_(std::move(other.rateLimits_))
    , rateLimitConfig_(other.rateLimitConfig_)
    , pinnedCertDer_(std::move(other.pinnedCertDer_))
    , masterKey_(std::move(other.masterKey_))
{}

SecurityManager& SecurityManager::operator=(SecurityManager&& other) noexcept {
    if (this != &other) {
        std::lock_guard<std::mutex> lock1(sessionTokensMutex_);
        std::lock_guard<std::mutex> lock2(deviceSessionsMutex_);
        std::lock_guard<std::mutex> lock3(messageSequencesMutex_);
        std::lock_guard<std::mutex> lock4(sessionNoncesMutex_);
        std::lock_guard<std::mutex> lock5(rateLimitsMutex_);
        std::lock_guard<std::mutex> lock6(pinnedCertMutex_);
        std::lock_guard<std::mutex> lock7(masterKeyMutex_);

        sessionTokens_ = std::move(other.sessionTokens_);
        deviceSessions_ = std::move(other.deviceSessions_);
        messageSequences_ = std::move(other.messageSequences_);
        sessionNonces_ = std::move(other.sessionNonces_);
        rateLimits_ = std::move(other.rateLimits_);
        rateLimitConfig_ = other.rateLimitConfig_;
        pinnedCertDer_ = std::move(other.pinnedCertDer_);
        masterKey_ = std::move(other.masterKey_);
    }
    return *this;
}

// ===== Existing validation methods (preserved) =====

bool SecurityManager::validateCameraId(const std::string& id) const {
    return !id.empty() && id.size() <= 128;
}

bool SecurityManager::validatePayloadSize(const std::string& payload) const {
    return payload.size() <= 65536;
}

bool SecurityManager::validateGainRange(float gain) const {
    return gain >= 0.0f && gain <= 4.0f;
}

// ===== Session Token Management =====

std::optional<SessionToken> SecurityManager::generateSessionToken(
    const std::string& sessionId,
    const std::string& deviceId,
    std::chrono::milliseconds ttl
) {
    if (sessionId.empty() || deviceId.empty() || sessionId.size() > 128 || deviceId.size() > 128) {
        return std::nullopt;
    }

    SessionToken token;
    token.sessionId = sessionId;
    token.deviceId = deviceId;
    token.issuedAt = currentTimeMs();
    token.expiresAt = token.issuedAt + ttl.count();

    // Generate cryptographically secure HMAC key (32 bytes for SHA-256)
    token.hmacKey = generateRandomBytes(32);

    // Generate session token: HMAC-SHA256(sessionId + deviceId + issuedAt, masterKey)
    std::string tokenData = sessionId + "|" + deviceId + "|" + std::to_string(token.issuedAt);
    unsigned int hmacLen = 0;
    std::vector<uint8_t> hmacResult(EVP_MAX_MD_SIZE);
    
    {
        std::lock_guard<std::mutex> lock(masterKeyMutex_);
        HMAC(EVP_sha256(), masterKey_.data(), static_cast<int>(masterKey_.size()),
             reinterpret_cast<const unsigned char*>(tokenData.c_str()), static_cast<int>(tokenData.size()),
             hmacResult.data(), &hmacLen);
    }
    
    hmacResult.resize(hmacLen);
    
    // Encode token as hex
    std::ostringstream oss;
    for (uint8_t b : hmacResult) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    token.token = oss.str();

    // Store token
    {
        std::lock_guard<std::mutex> lock(sessionTokensMutex_);
        sessionTokens_[sessionId] = token;
    }

    // Register pairing
    registerPairing(deviceId, sessionId);

    return token;
}

bool SecurityManager::validateSessionToken(const SessionToken& token) const {
    if (token.sessionId.empty() || token.deviceId.empty() || token.token.empty()) {
        return false;
    }

    // Check expiration
    int64_t now = currentTimeMs();
    if (now > token.expiresAt) {
        return false;
    }

    // Verify token format (should be hex encoded)
    if (token.token.size() != 64) { // SHA-256 = 32 bytes = 64 hex chars
        return false;
    }

    // Check if session still exists and token matches (revoked sessions should be invalid)
    {
        std::lock_guard<std::mutex> lock(sessionTokensMutex_);
        auto it = sessionTokens_.find(token.sessionId);
        if (it == sessionTokens_.end()) {
            return false; // Session revoked or doesn't exist
        }
        // Also verify the token matches the current stored token
        if (it->second.token != token.token) {
            return false; // Token was refreshed/replaced
        }
    }

    // Verify token cryptographically using persistent master key
    std::string tokenData = token.sessionId + "|" + token.deviceId + "|" + std::to_string(token.issuedAt);
    
    unsigned int hmacLen = 0;
    std::vector<uint8_t> expectedHmac(EVP_MAX_MD_SIZE);
    
    {
        std::lock_guard<std::mutex> lock(masterKeyMutex_);
        HMAC(EVP_sha256(), masterKey_.data(), static_cast<int>(masterKey_.size()),
             reinterpret_cast<const unsigned char*>(tokenData.c_str()), static_cast<int>(tokenData.size()),
             expectedHmac.data(), &hmacLen);
    }
    expectedHmac.resize(hmacLen);

    // Compare with provided token
    std::ostringstream oss;
    for (uint8_t b : expectedHmac) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    
    return token.token == oss.str();
}

// Simpler interface for IPC: validate token by sessionId and token string
bool SecurityManager::validateSessionToken(const std::string& sessionId, const std::string& token) const {
    if (sessionId.empty() || token.empty()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(sessionTokensMutex_);
    auto it = sessionTokens_.find(sessionId);
    if (it == sessionTokens_.end()) {
        return false; // Session revoked or doesn't exist
    }
    
    // Check expiration
    int64_t now = currentTimeMs();
    if (now > it->second.expiresAt) {
        return false;
    }
    
    // Verify token matches stored token
    return it->second.token == token;
}

bool SecurityManager::verifySessionOwnership(const std::string& sessionId, const std::string& deviceId) const {
    std::lock_guard<std::mutex> lock(sessionTokensMutex_);
    auto it = sessionTokens_.find(sessionId);
    if (it == sessionTokens_.end()) {
        return false;
    }
    return it->second.deviceId == deviceId;
}

void SecurityManager::revokeSessionToken(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(sessionTokensMutex_);
    auto it = sessionTokens_.find(sessionId);
    if (it != sessionTokens_.end()) {
        // Also remove from device sessions
        const std::string& deviceId = it->second.deviceId;
        {
            std::lock_guard<std::mutex> lockDev(deviceSessionsMutex_);
            auto devIt = deviceSessions_.find(deviceId);
            if (devIt != deviceSessions_.end()) {
                devIt->second.erase(sessionId);
                if (devIt->second.empty()) {
                    deviceSessions_.erase(devIt);
                }
            }
        }
        sessionTokens_.erase(it);
    }
    
    // Clean up associated data
    {
        std::lock_guard<std::mutex> lockSeq(messageSequencesMutex_);
        messageSequences_.erase(sessionId);
    }
    {
        std::lock_guard<std::mutex> lockNonce(sessionNoncesMutex_);
        sessionNonces_.erase(sessionId);
    }
    {
        std::lock_guard<std::mutex> lockRate(rateLimitsMutex_);
        rateLimits_.erase(sessionId);
    }
}

std::optional<SessionToken> SecurityManager::refreshSessionToken(const std::string& sessionId) {
    SessionToken oldToken;
    {
        std::lock_guard<std::mutex> lock(sessionTokensMutex_);
        auto it = sessionTokens_.find(sessionId);
        if (it == sessionTokens_.end()) {
            return std::nullopt;
        }
        oldToken = it->second;
    }
    
    // Generate new token with extended TTL (without holding lock to avoid deadlock)
    auto newTokenOpt = generateSessionToken(sessionId, oldToken.deviceId, std::chrono::hours(24));
    if (newTokenOpt) {
        // Preserve the original HMAC key for message continuity (or generate new one)
        newTokenOpt->hmacKey = oldToken.hmacKey;
        {
            std::lock_guard<std::mutex> lock(sessionTokensMutex_);
            sessionTokens_[sessionId] = *newTokenOpt;
        }
    }
    
    return newTokenOpt;
}

// Overload that allows specifying TTL
std::optional<SessionToken> SecurityManager::refreshSessionToken(const std::string& sessionId, std::chrono::milliseconds ttl) {
    SessionToken oldToken;
    {
        std::lock_guard<std::mutex> lock(sessionTokensMutex_);
        auto it = sessionTokens_.find(sessionId);
        if (it == sessionTokens_.end()) {
            return std::nullopt;
        }
        oldToken = it->second;
    }
    
    // Generate new token with specified TTL (without holding lock to avoid deadlock)
    auto newTokenOpt = generateSessionToken(sessionId, oldToken.deviceId, ttl);
    if (newTokenOpt) {
        // Preserve the original HMAC key for message continuity
        newTokenOpt->hmacKey = oldToken.hmacKey;
        {
            std::lock_guard<std::mutex> lock(sessionTokensMutex_);
            sessionTokens_[sessionId] = *newTokenOpt;
        }
    }
    
    return newTokenOpt;
}

// ===== Message Signing/Verification =====

std::string SecurityManager::signMessage(const ProtocolMessage& message, const std::vector<uint8_t>& hmacKey) const {
    // Create canonical message representation for signing
    // Format: version|messageId|sessionId|deviceId|timestamp|command|payload|nonce
    std::ostringstream oss;
    oss << static_cast<uint16_t>(message.version) << "|"
        << message.messageId << "|"
        << message.sessionId << "|"
        << message.deviceId << "|"
        << message.timestamp << "|"
        << static_cast<uint16_t>(message.command) << "|"
        << message.payload << "|"
        << message.nonce;
    
    std::string canonicalMessage = oss.str();
    
    unsigned int hmacLen = 0;
    std::vector<uint8_t> hmacResult(EVP_MAX_MD_SIZE);
    
    HMAC(EVP_sha256(), hmacKey.data(), static_cast<int>(hmacKey.size()),
         reinterpret_cast<const unsigned char*>(canonicalMessage.c_str()), static_cast<int>(canonicalMessage.size()),
         hmacResult.data(), &hmacLen);
    hmacResult.resize(hmacLen);

    // Encode as hex
    std::ostringstream hexOss;
    for (uint8_t b : hmacResult) {
        hexOss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    
    return hexOss.str();
}

bool SecurityManager::verifyMessageSignature(const ProtocolMessage& message, const std::vector<uint8_t>& hmacKey) const {
    if (message.signature.empty() || message.signature.size() != 64) {
        return false;
    }
    
    std::string expectedSignature = signMessage(message, hmacKey);
    
    // Constant-time comparison to prevent timing attacks
    if (expectedSignature.size() != message.signature.size()) {
        return false;
    }
    
    int result = 0;
    for (size_t i = 0; i < expectedSignature.size(); ++i) {
        result |= expectedSignature[i] ^ message.signature[i];
    }
    
    return result == 0;
}

// ===== Protocol Validation =====

phonecam::ValidationResult SecurityManager::validateProtocolMessage(const std::string& rawMessage) {
    phonecam::ValidationResult result;
    
    // Basic size check
    if (rawMessage.empty() || rawMessage.size() > 65536) {
        result.errorMessage = "Message size invalid";
        return result;
    }

    // Parse message as JSON
    json parsed;
    try {
        parsed = json::parse(rawMessage);
    } catch (const json::parse_error&) {
        result.errorMessage = "Invalid JSON format";
        return result;
    }

    ProtocolMessage msg;
    
    // Extract required fields from JSON
    if (!parsed.contains("protocolVersion") || !parsed.contains("messageId") ||
        !parsed.contains("sessionId") || !parsed.contains("deviceId") ||
        !parsed.contains("timestamp") || !parsed.contains("command")) {
        result.errorMessage = "Missing required fields";
        return result;
    }
    
    try {
        msg.version = static_cast<ProtocolVersion>(parsed["protocolVersion"].get<uint16_t>());
        msg.messageId = parsed["messageId"].get<uint64_t>();
        msg.sessionId = parsed["sessionId"].get<std::string>();
        msg.deviceId = parsed["deviceId"].get<std::string>();
        msg.timestamp = parsed["timestamp"].get<int64_t>();
        msg.command = static_cast<CommandType>(parsed["command"].get<uint16_t>());
        
        // Optional fields
        if (parsed.contains("payload")) {
            // Store payload as JSON string for downstream processing
            msg.payload = parsed["payload"].dump();
        }
        if (parsed.contains("signature")) {
            msg.signature = parsed["signature"].get<std::string>();
        }
        if (parsed.contains("nonce")) {
            msg.nonce = parsed["nonce"].get<std::string>();
        }
    } catch (const json::exception&) {
        result.errorMessage = "Invalid field format";
        return result;
    }
    
    // Validate protocol version
    if (!validateProtocolVersion(msg.version)) {
        result.errorMessage = "Unsupported protocol version";
        return result;
    }
    
    // Validate timestamp
    if (!validateTimestamp(msg.timestamp)) {
        result.errorMessage = "Timestamp outside acceptable window";
        return result;
    }
    
    // Validate command
    if (!validateCommand(msg.command)) {
        result.errorMessage = "Invalid or unauthorized command";
        return result;
    }
    
    // Parse payload JSON for schema validation
    json payloadJson;
    if (!msg.payload.empty()) {
        try {
            payloadJson = json::parse(msg.payload);
        } catch (const json::parse_error&) {
            result.errorMessage = "Invalid payload JSON format";
            return result;
        }
    }
    
    // Validate payload schema using JSON schema validation
    if (!validatePayloadJsonSchema(msg.command, payloadJson)) {
        result.errorMessage = "Payload schema validation failed";
        return result;
    }
    
    // Validate payload size for command
    if (msg.payload.size() > getMaxPayloadSize(msg.command)) {
        result.errorMessage = "Payload exceeds maximum size for command";
        return result;
    }
    
    // Validate message sequence
    if (!validateMessageSequence(msg.sessionId, msg.messageId)) {
        result.errorMessage = "Invalid message sequence";
        return result;
    }
    
    // Validate nonce (replay protection)
    if (!checkAndRecordNonce(msg.sessionId, msg.nonce)) {
        result.errorMessage = "Replay detected or invalid nonce";
        return result;
    }
    
    // Validate session exists and device owns it
    if (!validateSessionExists(msg.sessionId)) {
        result.errorMessage = "Session not found";
        return result;
    }
    
    if (!verifySessionOwnership(msg.sessionId, msg.deviceId)) {
        result.errorMessage = "Device does not own session";
        return result;
    }
    
    // Verify signature if present
    if (!msg.signature.empty()) {
        std::lock_guard<std::mutex> lock(sessionTokensMutex_);
        auto tokenIt = sessionTokens_.find(msg.sessionId);
        if (tokenIt == sessionTokens_.end()) {
            result.errorMessage = "Session token not found for signature verification";
            return result;
        }
        
        if (!verifyMessageSignature(msg, tokenIt->second.hmacKey)) {
            result.errorMessage = "Message signature verification failed";
            return result;
        }
    }
    
    // Check rate limit
    if (!checkRateLimit(msg.sessionId, msg.payload.size())) {
        result.errorMessage = "Rate limit exceeded";
        return result;
    }
    
    result.valid = true;
    result.parsedMessage = msg;
    return result;
}

bool SecurityManager::validateProtocolVersion(ProtocolVersion version) const {
    return version == ProtocolVersion::V1;
}

bool SecurityManager::validateMessageSequence(const std::string& sessionId, uint64_t messageId) {
    std::lock_guard<std::mutex> lock(messageSequencesMutex_);
    
    auto it = messageSequences_.find(sessionId);
    if (it == messageSequences_.end()) {
        // First message for this session
        messageSequences_[sessionId] = messageId;
        return true;
    }
    
    // Message ID must be strictly increasing
    if (messageId <= it->second) {
        return false;
    }
    
    // Check for large gaps (potential replay or attack)
    if (messageId > it->second + 1000) {
        // Allow but log warning - could be reconnection
    }
    
    it->second = messageId;
    return true;
}

bool SecurityManager::validateTimestamp(int64_t timestamp, std::chrono::milliseconds window) const {
    int64_t now = currentTimeMs();
    int64_t diff = std::llabs(now - timestamp);
    return diff <= window.count();
}

bool SecurityManager::validateCommand(CommandType command) const {
    return std::find(allowedCommands_.begin(), allowedCommands_.end(), command) != allowedCommands_.end();
}

bool SecurityManager::validatePayloadSchema(CommandType command, const std::string& payload) const {
    // Empty payload is valid for commands that don't require parameters
    if (payload.empty()) {
        switch (command) {
            case CommandType::PING:
            case CommandType::PONG:
            case CommandType::VIDEO_START:
            case CommandType::VIDEO_STOP:
            case CommandType::AUDIO_START:
            case CommandType::AUDIO_STOP:
            case CommandType::AUDIO_MUTE:
            case CommandType::AUDIO_UNMUTE:
            case CommandType::STATS_REQUEST:
                return true;
            default:
                return false;
        }
    }
    
    // Try to parse as JSON
    try {
        json payloadJson = json::parse(payload);
        return validatePayloadJsonSchema(command, payloadJson);
    } catch (const json::parse_error&) {
        return false;
    }
}

bool SecurityManager::validatePayloadJsonSchema(CommandType command, const json& payload) const {
    // Empty payload is valid for commands that don't require parameters
    if (payload.is_null() || payload.empty()) {
        switch (command) {
            case CommandType::PING:
            case CommandType::PONG:
            case CommandType::VIDEO_START:
            case CommandType::VIDEO_STOP:
            case CommandType::AUDIO_START:
            case CommandType::AUDIO_STOP:
            case CommandType::AUDIO_MUTE:
            case CommandType::AUDIO_UNMUTE:
            case CommandType::STATS_REQUEST:
                return true;
            default:
                return false;
        }
    }
    
    // Command-specific JSON schema validation
    switch (command) {
        case CommandType::VIDEO_CONFIG: {
            // Expect: {"resolution":"1920x1080","fps":30,"bitrate":5000000,"codec":"H264"}
            if (!payload.is_object()) return false;
            if (!payload.contains("resolution") || !payload["resolution"].is_string()) return false;
            if (!payload.contains("fps") || !payload["fps"].is_number_integer()) return false;
            if (!payload.contains("bitrate") || !payload["bitrate"].is_number_integer()) return false;
            
            // Validate ranges
            int fps = payload["fps"].get<int>();
            int bitrate = payload["bitrate"].get<int>();
            if (fps <= 0 || fps > 120) return false;
            if (bitrate <= 0 || bitrate > 50000000) return false; // Max 50 Mbps
            
            // Validate resolution format (WxH)
            const std::string& resolution = payload["resolution"].get<std::string>();
            if (resolution.find('x') == std::string::npos) return false;
            
            return true;
        }
        case CommandType::AUDIO_CONFIG: {
            if (!payload.is_object()) return false;
            if (payload.contains("sampleRate")) {
                int sr = payload["sampleRate"].get<int>();
                if (sr != 8000 && sr != 16000 && sr != 44100 && sr != 48000) return false;
            }
            if (payload.contains("channels")) {
                int ch = payload["channels"].get<int>();
                if (ch != 1 && ch != 2) return false;
            }
            return true;
        }
        case CommandType::AUDIO_GAIN: {
            // Expect: {"gain":1.5}
            if (!payload.is_object()) return false;
            if (!payload.contains("gain") || !payload["gain"].is_number()) return false;
            double gain = payload["gain"].get<double>();
            return gain >= 0.0 && gain <= 4.0;
        }
        case CommandType::AUDIO_DELAY: {
            // Expect: {"delayMs":100}
            if (!payload.is_object()) return false;
            if (!payload.contains("delayMs") || !payload["delayMs"].is_number_integer()) return false;
            int delay = payload["delayMs"].get<int>();
            return delay >= 0 && delay <= 5000; // Max 5 second delay
        }
        case CommandType::ZOOM_SET: {
            // Expect: {"zoom":2.0}
            if (!payload.is_object()) return false;
            if (!payload.contains("zoom") || !payload["zoom"].is_number()) return false;
            double zoom = payload["zoom"].get<double>();
            return zoom >= 1.0 && zoom <= 10.0;
        }
        case CommandType::CAMERA_SWITCH: {
            // Expect: {"cameraId":"front"} or {"cameraId":"back"}
            if (!payload.is_object()) return false;
            if (!payload.contains("cameraId") || !payload["cameraId"].is_string()) return false;
            std::string cameraId = payload["cameraId"].get<std::string>();
            return cameraId == "front" || cameraId == "back";
        }
        case CommandType::SESSION_CREATE: {
            // Expect: {"deviceId":"...","capabilities":[...]}
            if (!payload.is_object()) return false;
            if (!payload.contains("deviceId") || !payload["deviceId"].is_string()) return false;
            if (payload.contains("capabilities")) {
                if (!payload["capabilities"].is_array()) return false;
            }
            return true;
        }
        case CommandType::PAIR_REQUEST: {
            // Expect: {"deviceId":"...","publicKey":"...","nonce":"..."}
            if (!payload.is_object()) return false;
            if (!payload.contains("deviceId") || !payload["deviceId"].is_string()) return false;
            if (!payload.contains("publicKey") || !payload["publicKey"].is_string()) return false;
            if (!payload.contains("nonce") || !payload["nonce"].is_string()) return false;
            return true;
        }
        case CommandType::KEY_EXCHANGE: {
            // Expect: {"publicKey":"...","keyType":"ECDH"}
            if (!payload.is_object()) return false;
            if (!payload.contains("publicKey") || !payload["publicKey"].is_string()) return false;
            if (payload.contains("keyType") && !payload["keyType"].is_string()) return false;
            return true;
        }
        case CommandType::AUTH_CHALLENGE: {
            // Expect: {"challenge":"..."}
            if (!payload.is_object()) return false;
            if (!payload.contains("challenge") || !payload["challenge"].is_string()) return false;
            return true;
        }
        case CommandType::AUTH_RESPONSE: {
            // Expect: {"response":"...","signature":"..."}
            if (!payload.is_object()) return false;
            if (!payload.contains("response") || !payload["response"].is_string()) return false;
            if (!payload.contains("signature") || !payload["signature"].is_string()) return false;
            return true;
        }
        case CommandType::TOKEN_REFRESH: {
            // Expect: {"sessionId":"..."}
            if (!payload.is_object()) return false;
            if (!payload.contains("sessionId") || !payload["sessionId"].is_string()) return false;
            return true;
        }
        case CommandType::STATS_RESPONSE: {
            // Flexible schema for stats
            return payload.is_object();
        }
        default:
            // For unknown commands with payload, allow but require valid JSON object
            return payload.is_object();
    }
}

size_t SecurityManager::getMaxPayloadSize(CommandType command) {
    switch (command) {
        case CommandType::SESSION_CREATE:
        case CommandType::PAIR_REQUEST:
        case CommandType::KEY_EXCHANGE:
            return 8192;  // 8 KB for key exchange
        case CommandType::VIDEO_CONFIG:
        case CommandType::AUDIO_CONFIG:
            return 4096;  // 4 KB for config
        case CommandType::STATS_RESPONSE:
            return 16384; // 16 KB for stats
        default:
            return 1024;  // 1 KB default
    }
}

bool SecurityManager::validateMessageStructure(const ProtocolMessage& msg) const {
    return !msg.sessionId.empty() && 
           !msg.deviceId.empty() &&
           msg.messageId > 0 &&
           validateProtocolVersion(msg.version) &&
           validateCommand(msg.command);
}

bool SecurityManager::validateSessionExists(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(sessionTokensMutex_);
    return sessionTokens_.find(sessionId) != sessionTokens_.end();
}

// ===== Replay Protection =====

bool SecurityManager::checkAndRecordNonce(const std::string& sessionId, const std::string& nonce) {
    if (nonce.empty() || nonce.size() > 64) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(sessionNoncesMutex_);
    
    auto& nonces = sessionNonces_[sessionId];
    auto now = std::chrono::steady_clock::now();
    
    // Clean up old nonces first
    constexpr auto maxAge = std::chrono::minutes(5);
    nonces.erase(
        std::remove_if(nonces.begin(), nonces.end(),
            [now, maxAge](const NonceEntry& entry) {
                return now - entry.timestamp > maxAge;
            }),
        nonces.end()
    );
    
    // Check if nonce already used
    for (const auto& entry : nonces) {
        if (entry.nonce == nonce) {
            return false; // Replay detected
        }
    }
    
    // Record new nonce
    nonces.push_back({nonce, now});
    
    // Limit nonce storage per session
    if (nonces.size() > 1000) {
        nonces.erase(nonces.begin(), nonces.begin() + 500);
    }
    
    return true;
}

void SecurityManager::cleanupNonces(std::chrono::milliseconds maxAge) {
    std::lock_guard<std::mutex> lock(sessionNoncesMutex_);
    auto now = std::chrono::steady_clock::now();
    
    for (auto& [sessionId, nonces] : sessionNonces_) {
        nonces.erase(
            std::remove_if(nonces.begin(), nonces.end(),
                [now, maxAge](const NonceEntry& entry) {
                    return now - entry.timestamp > maxAge;
                }),
            nonces.end()
        );
    }
    
    // Remove empty entries
    for (auto it = sessionNonces_.begin(); it != sessionNonces_.end();) {
        if (it->second.empty()) {
            it = sessionNonces_.erase(it);
        } else {
            ++it;
        }
    }
}

// ===== Rate Limiting =====

bool SecurityManager::checkRateLimit(const std::string& sessionId, size_t payloadSize) {
    std::lock_guard<std::mutex> lock(rateLimitsMutex_);
    
    auto& limit = rateLimits_[sessionId];
    auto now = std::chrono::steady_clock::now();
    
    // Reset window if needed (1 second windows)
    constexpr auto windowDuration = std::chrono::seconds(1);
    if (now - limit.windowStart >= windowDuration) {
        limit.windowStart = now;
        limit.messageCount = 0;
        limit.byteCount = 0;
    }
    
    // Check limits - burstAllowance is additional messages beyond maxMessagesPerSecond
    size_t maxMessages = rateLimitConfig_.maxMessagesPerSecond + rateLimitConfig_.burstAllowance;
    size_t maxBytes = rateLimitConfig_.maxPayloadBytesPerSecond + rateLimitConfig_.burstAllowance * 1024;
    
    if (limit.messageCount >= maxMessages) {
        return false;
    }
    
    if (limit.byteCount + payloadSize > maxBytes) {
        return false;
    }
    
    limit.messageCount++;
    limit.byteCount += payloadSize;
    
    return true;
}

void SecurityManager::setRateLimitConfig(const phonecam::RateLimitConfig& config) {
    std::lock_guard<std::mutex> lock(rateLimitsMutex_);
    rateLimitConfig_ = config;
}

phonecam::RateLimitConfig SecurityManager::getRateLimitConfig() const {
    std::lock_guard<std::mutex> lock(rateLimitsMutex_);
    return rateLimitConfig_;
}

// ===== TLS Certificate Pinning =====

void SecurityManager::setPinnedCertificate(const std::vector<uint8_t>& certDer) {
    std::lock_guard<std::mutex> lock(pinnedCertMutex_);
    pinnedCertDer_ = certDer;
}

bool SecurityManager::verifyPinnedCertificate(const std::vector<uint8_t>& peerCertDer) const {
    std::lock_guard<std::mutex> lock(pinnedCertMutex_);
    
    if (pinnedCertDer_.empty()) {
        // No pinned certificate set - allow (but log warning in production)
        return true;
    }
    
    // Simple binary comparison for exact match
    // In production, compare SPKI (Subject Public Key Info) for more flexibility
    return pinnedCertDer_ == peerCertDer;
}

bool SecurityManager::verifyPinnedCertificate(const ::X509* peerCert) const {
    if (!peerCert) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(pinnedCertMutex_);
    
    if (pinnedCertDer_.empty()) {
        return true;
    }
    
    // Convert peer cert to DER
    int len = i2d_X509(peerCert, nullptr);
    if (len <= 0) {
        return false;
    }
    
    std::vector<uint8_t> peerDer(len);
    unsigned char* p = peerDer.data();
    len = i2d_X509(peerCert, &p);
    if (len <= 0) {
        return false;
    }
    peerDer.resize(len);
    
    return verifyPinnedCertificate(peerDer);
}

// ===== Device/Session Registry =====

void SecurityManager::registerPairing(const std::string& deviceId, const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(deviceSessionsMutex_);
    deviceSessions_[deviceId].insert(sessionId);
}

bool SecurityManager::isValidPairing(const std::string& deviceId, const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(deviceSessionsMutex_);
    auto it = deviceSessions_.find(deviceId);
    if (it == deviceSessions_.end()) {
        return false;
    }
    return it->second.find(sessionId) != it->second.end();
}

void SecurityManager::removePairing(const std::string& deviceId, const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(deviceSessionsMutex_);
    auto it = deviceSessions_.find(deviceId);
    if (it != deviceSessions_.end()) {
        it->second.erase(sessionId);
        if (it->second.empty()) {
            deviceSessions_.erase(it);
        }
    }
}

// ===== Utility =====

std::vector<uint8_t> SecurityManager::generateRandomBytes(size_t count) {
    std::vector<uint8_t> bytes(count);
    if (RAND_bytes(bytes.data(), static_cast<int>(count)) != 1) {
        // Fallback - should not happen in production
        for (size_t i = 0; i < count; ++i) {
            bytes[i] = static_cast<uint8_t>(rand());
        }
    }
    return bytes;
}

std::string SecurityManager::generateNonce(size_t length) {
    auto bytes = generateRandomBytes(length);
    std::ostringstream oss;
    for (uint8_t b : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

int64_t SecurityManager::currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

} // namespace phonecam