#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <nlohmann/json.hpp>

// OpenSSL headers needed for X509 type
#include <openssl/evp.h>
#include <openssl/x509.h>

namespace phonecam {

using json = nlohmann::json;

enum class ProtocolVersion : uint16_t {
    V1 = 1
};

enum class CommandType : uint16_t {
    // Session management
    PAIR_REQUEST = 0x0100,
    PAIR_RESPONSE = 0x0101,
    SESSION_CREATE = 0x0102,
    SESSION_DESTROY = 0x0103,
    SESSION_RECONNECT = 0x0104,
    
    // Video control
    VIDEO_START = 0x0200,
    VIDEO_STOP = 0x0201,
    VIDEO_CONFIG = 0x0202,
    CAMERA_SWITCH = 0x0203,
    TORCH_TOGGLE = 0x0204,
    ZOOM_SET = 0x0205,
    FOCUS_SET = 0x0206,
    EXPOSURE_SET = 0x0207,
    
    // Audio control
    AUDIO_START = 0x0300,
    AUDIO_STOP = 0x0301,
    AUDIO_MUTE = 0x0302,
    AUDIO_UNMUTE = 0x0303,
    AUDIO_GAIN = 0x0304,
    AUDIO_DELAY = 0x0305,
    AUDIO_CONFIG = 0x0306,
    
    // Diagnostics
    PING = 0x0400,
    PONG = 0x0401,
    STATS_REQUEST = 0x0402,
    STATS_RESPONSE = 0x0403,
    
    // Security
    KEY_EXCHANGE = 0x0500,
    AUTH_CHALLENGE = 0x0501,
    AUTH_RESPONSE = 0x0502,
    TOKEN_REFRESH = 0x0503
};

struct ProtocolMessage {
    ProtocolVersion version;
    uint64_t messageId;
    std::string sessionId;
    std::string deviceId;
    int64_t timestamp;  // Unix milliseconds
    CommandType command;
    std::string payload;
    std::string signature;  // HMAC-SHA256 of message
    std::string nonce;      // Client-generated nonce for replay protection
};

struct SessionToken {
    std::string sessionId;
    std::string deviceId;
    std::string token;          // Cryptographic session token
    int64_t issuedAt;           // Unix milliseconds
    int64_t expiresAt;          // Unix milliseconds
    std::vector<uint8_t> hmacKey;  // HMAC key for message signing
};

struct RateLimitConfig {
    size_t maxMessagesPerSecond = 100;
    size_t maxPayloadBytesPerSecond = 1024 * 1024;  // 1 MB/s
    size_t burstAllowance = 10;
};

struct ValidationResult {
    bool valid = false;
    std::string errorMessage;
    std::optional<ProtocolMessage> parsedMessage;
};

class SecurityManager {
public:
    SecurityManager();
    ~SecurityManager();

    // Non-copyable, movable
    SecurityManager(const SecurityManager&) = delete;
    SecurityManager& operator=(const SecurityManager&) = delete;
    SecurityManager(SecurityManager&&) noexcept;
    SecurityManager& operator=(SecurityManager&&) noexcept;

    // ===== Existing validation methods (preserved) =====
    bool validateCameraId(const std::string& id) const;
    bool validatePayloadSize(const std::string& payload) const;
    bool validateGainRange(float gain) const;

    // ===== Session Token Management =====
    // Generate a new session token for a device/session pair
    std::optional<SessionToken> generateSessionToken(
        const std::string& sessionId,
        const std::string& deviceId,
        std::chrono::milliseconds ttl = std::chrono::hours(24)
    );

    // Validate a session token cryptographically (full token struct)
    bool validateSessionToken(const SessionToken& token) const;

    // Validate a session token by sessionId and token string (simpler interface for IPC)
    bool validateSessionToken(const std::string& sessionId, const std::string& token) const;

    // Verify session ownership (device owns session)
    bool verifySessionOwnership(const std::string& sessionId, const std::string& deviceId) const;

    // Revoke a session token
    void revokeSessionToken(const std::string& sessionId);

    // Refresh session token (extend TTL)
    std::optional<SessionToken> refreshSessionToken(const std::string& sessionId);
    
    // Refresh session token with custom TTL
    std::optional<SessionToken> refreshSessionToken(const std::string& sessionId, std::chrono::milliseconds ttl);

    // ===== Message Signing/Verification =====
    // Sign a protocol message with HMAC-SHA256
    std::string signMessage(const ProtocolMessage& message, const std::vector<uint8_t>& hmacKey) const;

    // Verify message signature
    bool verifyMessageSignature(const ProtocolMessage& message, const std::vector<uint8_t>& hmacKey) const;

    // ===== Protocol Validation =====
    // Full protocol message validation
    ValidationResult validateProtocolMessage(const std::string& rawMessage);

    // Validate protocol version
    bool validateProtocolVersion(ProtocolVersion version) const;

    // Validate message ID sequencing (monotonic increasing per session)
    bool validateMessageSequence(const std::string& sessionId, uint64_t messageId);

    // Validate timestamp (within acceptable window)
    bool validateTimestamp(int64_t timestamp, std::chrono::milliseconds window = std::chrono::seconds(30)) const;

    // Validate command against allowlist
    bool validateCommand(CommandType command) const;

    // Validate payload schema per command type
    bool validatePayloadSchema(CommandType command, const std::string& payload) const;

    // ===== Replay Protection =====
    // Check and record nonce (sliding window)
    bool checkAndRecordNonce(const std::string& sessionId, const std::string& nonce);

    // Clean up old nonces (call periodically)
    void cleanupNonces(std::chrono::milliseconds maxAge = std::chrono::minutes(5));

    // ===== Rate Limiting =====
    // Check rate limit for session
    bool checkRateLimit(const std::string& sessionId, size_t payloadSize);

    // Configure rate limiting
    void setRateLimitConfig(const RateLimitConfig& config);
    RateLimitConfig getRateLimitConfig() const;

    // ===== TLS Certificate Pinning =====
    // Set pinned certificate (DER encoded)
    void setPinnedCertificate(const std::vector<uint8_t>& certDer);

    // Verify peer certificate against pinned certificate
    bool verifyPinnedCertificate(const std::vector<uint8_t>& peerCertDer) const;

    // Verify peer certificate from OpenSSL X509
    bool verifyPinnedCertificate(const ::X509* peerCert) const;

    // Whether certificate pinning is actively enforced (i.e. a certificate
    // has been pinned via setPinnedCertificate). Callers that establish TLS
    // connections MUST check this before treating an empty pin list as "no
    // pinning configured, allow anything" - verifyPinnedCertificate() no
    // longer fails open silently; see its implementation.
    bool isCertificatePinningEnforced() const;

    // ===== Device/Session Registry =====
    // Register a device-session pairing
    void registerPairing(const std::string& deviceId, const std::string& sessionId);

    // Check if device-session pairing is valid
    bool isValidPairing(const std::string& deviceId, const std::string& sessionId) const;

    // Remove pairing
    void removePairing(const std::string& deviceId, const std::string& sessionId);

    // ===== Utility =====
    // Generate cryptographically secure random bytes
    static std::vector<uint8_t> generateRandomBytes(size_t count);

    // Generate secure nonce
    static std::string generateNonce(size_t length = 32);

    // Current time in milliseconds
    static int64_t currentTimeMs();

private:
    struct SessionRateLimit {
        std::chrono::steady_clock::time_point windowStart;
        size_t messageCount = 0;
        size_t byteCount = 0;
    };

    struct NonceEntry {
        std::string nonce;
        std::chrono::steady_clock::time_point timestamp;
    };

    // Session tokens (sessionId -> token)
    std::unordered_map<std::string, SessionToken> sessionTokens_;
    mutable std::mutex sessionTokensMutex_;

    // Device-session pairings
    std::unordered_map<std::string, std::unordered_set<std::string>> deviceSessions_;
    mutable std::mutex deviceSessionsMutex_;

    // Message ID tracking per session (sessionId -> last messageId)
    std::unordered_map<std::string, uint64_t> messageSequences_;
    mutable std::mutex messageSequencesMutex_;

    // Nonce tracking per session (sessionId -> list of nonces)
    std::unordered_map<std::string, std::vector<NonceEntry>> sessionNonces_;
    mutable std::mutex sessionNoncesMutex_;

    // Rate limiting per session
    std::unordered_map<std::string, SessionRateLimit> rateLimits_;
    mutable std::mutex rateLimitsMutex_;
    RateLimitConfig rateLimitConfig_;

    // Pinned certificate (DER encoded)
    std::vector<uint8_t> pinnedCertDer_;
    mutable std::mutex pinnedCertMutex_;

    // Allowed commands
    static const std::vector<CommandType> allowedCommands_;

    // Maximum payload size per command type
    static size_t getMaxPayloadSize(CommandType command);

    // JSON schema validation for command payloads
    bool validatePayloadJsonSchema(CommandType command, const json& payload) const;

    // Internal validation helpers
    bool validateMessageStructure(const ProtocolMessage& msg) const;
    bool validateSessionExists(const std::string& sessionId) const;

    // Master key for token signing (loaded from secure storage in production)
    std::vector<uint8_t> masterKey_;
    mutable std::mutex masterKeyMutex_;
};

} // namespace phonecam