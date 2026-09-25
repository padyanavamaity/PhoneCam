#include "phonecam/SecurityManager.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <cassert>

using namespace phonecam;

// Test helper macros
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAIL: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } else { \
            std::cout << "PASS: " << message << std::endl; \
        } \
    } while (0)

#define TEST_ASSERT_EQ(expected, actual, message) \
    do { \
        if ((expected) != (actual)) { \
            std::cerr << "FAIL: " << message << " - Expected: " << (expected) << ", Got: " << (actual) << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } else { \
            std::cout << "PASS: " << message << std::endl; \
        } \
    } while (0)

bool testMasterKeyPersistence() {
    std::cout << "\n=== Testing Master Key Persistence ===" << std::endl;
    
    SecurityManager sm;
    
    // Generate a session token
    auto tokenOpt = sm.generateSessionToken("test-session-1", "test-device-1", std::chrono::hours(1));
    TEST_ASSERT(tokenOpt.has_value(), "Token generation should succeed");
    
    SessionToken token = tokenOpt.value();
    TEST_ASSERT(!token.token.empty(), "Token should not be empty");
    TEST_ASSERT(token.token.size() == 64, "Token should be 64 hex chars (32 bytes SHA-256)");
    TEST_ASSERT(token.sessionId == "test-session-1", "Session ID should match");
    TEST_ASSERT(token.deviceId == "test-device-1", "Device ID should match");
    
    // Validate the token - this would fail if master key was regenerated
    bool valid = sm.validateSessionToken(token);
    TEST_ASSERT(valid, "Token validation should succeed with persistent master key");
    
    // Refresh token
    auto refreshedOpt = sm.refreshSessionToken("test-session-1");
    TEST_ASSERT(refreshedOpt.has_value(), "Token refresh should succeed");
    
    // Old token should still be valid (new token replaces old)
    bool refreshedValid = sm.validateSessionToken(refreshedOpt.value());
    TEST_ASSERT(refreshedValid, "Refreshed token should be valid");
    
    // Revoke and verify
    sm.revokeSessionToken("test-session-1");
    valid = sm.validateSessionToken(token);
    TEST_ASSERT(!valid, "Revoked token should be invalid");
    
    return true;
}

bool testJSONParsing() {
    std::cout << "\n=== Testing JSON Parsing ===" << std::endl;
    
    SecurityManager sm;
    
    // Create a valid JSON protocol message
    json msg = {
        {"protocolVersion", 1},
        {"messageId", 1},
        {"sessionId", "test-session-1"},
        {"deviceId", "test-device-1"},
        {"timestamp", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())},
        {"command", static_cast<uint16_t>(CommandType::VIDEO_CONFIG)},
        {"payload", {
            {"resolution", "1920x1080"},
            {"fps", 30},
            {"bitrate", 5000000},
            {"codec", "H264"}
        }},
        {"nonce", "test-nonce-1"}
    };
    
    std::string rawMessage = msg.dump();
    
    // First register a session
    sm.generateSessionToken("test-session-1", "test-device-1", std::chrono::hours(1));
    
    // Validate the message
    ValidationResult result = sm.validateProtocolMessage(rawMessage);
    TEST_ASSERT(result.valid, "Valid JSON message should pass validation");
    TEST_ASSERT(result.parsedMessage.has_value(), "Parsed message should be present");
    TEST_ASSERT(result.parsedMessage->command == CommandType::VIDEO_CONFIG, "Command should be VIDEO_CONFIG");
    
    // Test invalid JSON
    std::string invalidJson = "{ invalid json }";
    result = sm.validateProtocolMessage(invalidJson);
    TEST_ASSERT(!result.valid, "Invalid JSON should fail validation");
    TEST_ASSERT(result.errorMessage.find("Invalid JSON") != std::string::npos, "Error should mention JSON format");
    
    // Test missing required fields
    json incomplete = {
        {"protocolVersion", 1},
        {"messageId", 1}
        // Missing sessionId, deviceId, timestamp, command
    };
    result = sm.validateProtocolMessage(incomplete.dump());
    TEST_ASSERT(!result.valid, "Message with missing fields should fail");
    TEST_ASSERT(result.errorMessage.find("Missing required fields") != std::string::npos, "Error should mention missing fields");
    
    // Test unsupported protocol version
    json badVersion = msg;
    badVersion["protocolVersion"] = 999;
    result = sm.validateProtocolMessage(badVersion.dump());
    TEST_ASSERT(!result.valid, "Unsupported protocol version should fail");
    
    return true;
}

bool testJSONSchemaValidation() {
    std::cout << "\n=== Testing JSON Schema Validation ===" << std::endl;
    
    SecurityManager sm;
    sm.generateSessionToken("test-session-1", "test-device-1", std::chrono::hours(1));
    
    // Test VIDEO_CONFIG - valid
    json videoConfig = {
        {"protocolVersion", 1},
        {"messageId", 1},
        {"sessionId", "test-session-1"},
        {"deviceId", "test-device-1"},
        {"timestamp", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())},
        {"command", static_cast<uint16_t>(CommandType::VIDEO_CONFIG)},
        {"payload", {
            {"resolution", "1920x1080"},
            {"fps", 30},
            {"bitrate", 5000000}
        }},
        {"nonce", "test-nonce-vc-1"}
    };
    
    ValidationResult result = sm.validateProtocolMessage(videoConfig.dump());
    TEST_ASSERT(result.valid, "Valid VIDEO_CONFIG should pass");
    
    // Test VIDEO_CONFIG - invalid fps (too high)
    json badFps = videoConfig;
    badFps["messageId"] = 2;
    badFps["nonce"] = "test-nonce-vc-2";
    badFps["payload"]["fps"] = 200;  // Invalid: > 120
    result = sm.validateProtocolMessage(badFps.dump());
    TEST_ASSERT(!result.valid, "VIDEO_CONFIG with invalid fps should fail");
    TEST_ASSERT(result.errorMessage.find("schema validation failed") != std::string::npos, "Error should mention schema");
    
    // Test VIDEO_CONFIG - invalid bitrate (too high)
    json badBitrate = videoConfig;
    badBitrate["messageId"] = 3;
    badBitrate["nonce"] = "test-nonce-vc-3";
    badBitrate["payload"]["bitrate"] = 100000000;  // Invalid: > 50 Mbps
    result = sm.validateProtocolMessage(badBitrate.dump());
    TEST_ASSERT(!result.valid, "VIDEO_CONFIG with invalid bitrate should fail");
    
    // Test VIDEO_CONFIG - missing resolution
    json missingRes = videoConfig;
    missingRes["messageId"] = 4;
    missingRes["nonce"] = "test-nonce-vc-4";
    missingRes["payload"].erase("resolution");
    result = sm.validateProtocolMessage(missingRes.dump());
    TEST_ASSERT(!result.valid, "VIDEO_CONFIG missing resolution should fail");
    
    // Test AUDIO_GAIN - valid
    json audioGain = {
        {"protocolVersion", 1},
        {"messageId", 5},
        {"sessionId", "test-session-1"},
        {"deviceId", "test-device-1"},
        {"timestamp", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())},
        {"command", static_cast<uint16_t>(CommandType::AUDIO_GAIN)},
        {"payload", {
            {"gain", 1.5}
        }},
        {"nonce", "test-nonce-ag-1"}
    };
    result = sm.validateProtocolMessage(audioGain.dump());
    TEST_ASSERT(result.valid, "Valid AUDIO_GAIN should pass");
    
    // Test AUDIO_GAIN - invalid gain (too high)
    json badGain = audioGain;
    badGain["messageId"] = 6;
    badGain["nonce"] = "test-nonce-ag-2";
    badGain["payload"]["gain"] = 5.0;  // Invalid: > 4.0
    result = sm.validateProtocolMessage(badGain.dump());
    TEST_ASSERT(!result.valid, "AUDIO_GAIN with invalid gain should fail");
    
    // Test AUDIO_DELAY - valid
    json audioDelay = {
        {"protocolVersion", 1},
        {"messageId", 7},
        {"sessionId", "test-session-1"},
        {"deviceId", "test-device-1"},
        {"timestamp", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())},
        {"command", static_cast<uint16_t>(CommandType::AUDIO_DELAY)},
        {"payload", {
            {"delayMs", 100}
        }},
        {"nonce", "test-nonce-ad-1"}
    };
    result = sm.validateProtocolMessage(audioDelay.dump());
    TEST_ASSERT(result.valid, "Valid AUDIO_DELAY should pass");
    
    // Test AUDIO_DELAY - invalid delay (too high)
    json badDelay = audioDelay;
    badDelay["messageId"] = 8;
    badDelay["nonce"] = "test-nonce-ad-2";
    badDelay["payload"]["delayMs"] = 10000;  // Invalid: > 5000
    result = sm.validateProtocolMessage(badDelay.dump());
    TEST_ASSERT(!result.valid, "AUDIO_DELAY with invalid delay should fail");
    
    // Test CAMERA_SWITCH - valid
    json cameraSwitch = {
        {"protocolVersion", 1},
        {"messageId", 9},
        {"sessionId", "test-session-1"},
        {"deviceId", "test-device-1"},
        {"timestamp", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())},
        {"command", static_cast<uint16_t>(CommandType::CAMERA_SWITCH)},
        {"payload", {
            {"cameraId", "back"}
        }},
        {"nonce", "test-nonce-cs-1"}
    };
    result = sm.validateProtocolMessage(cameraSwitch.dump());
    TEST_ASSERT(result.valid, "Valid CAMERA_SWITCH should pass");
    
    // Test CAMERA_SWITCH - invalid cameraId
    json badCamera = cameraSwitch;
    badCamera["messageId"] = 10;
    badCamera["nonce"] = "test-nonce-cs-2";
    badCamera["payload"]["cameraId"] = "side";  // Invalid
    result = sm.validateProtocolMessage(badCamera.dump());
    TEST_ASSERT(!result.valid, "CAMERA_SWITCH with invalid cameraId should fail");
    
    // Test ZOOM_SET - valid
    json zoomSet = {
        {"protocolVersion", 1},
        {"messageId", 11},
        {"sessionId", "test-session-1"},
        {"deviceId", "test-device-1"},
        {"timestamp", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())},
        {"command", static_cast<uint16_t>(CommandType::ZOOM_SET)},
        {"payload", {
            {"zoom", 2.0}
        }},
        {"nonce", "test-nonce-zs-1"}
    };
    result = sm.validateProtocolMessage(zoomSet.dump());
    TEST_ASSERT(result.valid, "Valid ZOOM_SET should pass");
    
    // Test ZOOM_SET - invalid zoom (too high)
    json badZoom = zoomSet;
    badZoom["messageId"] = 12;
    badZoom["nonce"] = "test-nonce-zs-2";
    badZoom["payload"]["zoom"] = 15.0;  // Invalid: > 10.0
    result = sm.validateProtocolMessage(badZoom.dump());
    TEST_ASSERT(!result.valid, "ZOOM_SET with invalid zoom should fail");
    
    // Test AUDIO_CONFIG - valid
    json audioConfig = {
        {"protocolVersion", 1},
        {"messageId", 13},
        {"sessionId", "test-session-1"},
        {"deviceId", "test-device-1"},
        {"timestamp", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())},
        {"command", static_cast<uint16_t>(CommandType::AUDIO_CONFIG)},
        {"payload", {
            {"sampleRate", 48000},
            {"channels", 2}
        }},
        {"nonce", "test-nonce-ac-1"}
    };
    result = sm.validateProtocolMessage(audioConfig.dump());
    TEST_ASSERT(result.valid, "Valid AUDIO_CONFIG should pass");
    
    // Test AUDIO_CONFIG - invalid sample rate
    json badSampleRate = audioConfig;
    badSampleRate["messageId"] = 14;
    badSampleRate["nonce"] = "test-nonce-ac-2";
    badSampleRate["payload"]["sampleRate"] = 96000;  // Not in allowed list
    result = sm.validateProtocolMessage(badSampleRate.dump());
    TEST_ASSERT(!result.valid, "AUDIO_CONFIG with invalid sample rate should fail");
    
    // Test commands without payload that should be valid
    json ping = {
        {"protocolVersion", 1},
        {"messageId", 15},
        {"sessionId", "test-session-1"},
        {"deviceId", "test-device-1"},
        {"timestamp", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())},
        {"command", static_cast<uint16_t>(CommandType::PING)},
        {"nonce", "test-nonce-ping-1"}
    };
    result = sm.validateProtocolMessage(ping.dump());
    TEST_ASSERT(result.valid, "PING without payload should pass");
    
    return true;
}

bool testReplayProtection() {
    std::cout << "\n=== Testing Replay Protection (Nonce) ===" << std::endl;
    
    SecurityManager sm;
    sm.generateSessionToken("test-session-replay", "test-device-replay", std::chrono::hours(1));
    
    int64_t timestamp = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    
    // First message with nonce
    json msg1 = {
        {"protocolVersion", 1},
        {"messageId", 1},
        {"sessionId", "test-session-replay"},
        {"deviceId", "test-device-replay"},
        {"timestamp", timestamp},
        {"command", static_cast<uint16_t>(CommandType::PING)},
        {"nonce", "unique-nonce-1"}
    };
    
    ValidationResult result = sm.validateProtocolMessage(msg1.dump());
    TEST_ASSERT(result.valid, "First message with nonce should pass");
    
    // Replay same nonce - should fail
    json msg2 = msg1;
    msg2["messageId"] = 2;
    result = sm.validateProtocolMessage(msg2.dump());
    TEST_ASSERT(!result.valid, "Replayed nonce should be rejected");
    TEST_ASSERT(result.errorMessage.find("Replay detected") != std::string::npos, "Error should mention replay");
    
    // Different nonce - should pass
    json msg3 = msg1;
    msg3["messageId"] = 3;
    msg3["nonce"] = "unique-nonce-2";
    result = sm.validateProtocolMessage(msg3.dump());
    TEST_ASSERT(result.valid, "New nonce should pass");
    
    // Test nonce length limit
    json msg4 = msg1;
    msg4["messageId"] = 4;
    msg4["nonce"] = std::string(100, 'a');  // Too long (> 64)
    result = sm.validateProtocolMessage(msg4.dump());
    TEST_ASSERT(!result.valid, "Nonce too long should fail");
    
    return true;
}

bool testMessageSequence() {
    std::cout << "\n=== Testing Message Sequence Validation ===" << std::endl;
    
    SecurityManager sm;
    sm.generateSessionToken("test-session-seq", "test-device-seq", std::chrono::hours(1));
    
    int64_t timestamp = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    
    // First message
    json msg1 = {
        {"protocolVersion", 1},
        {"messageId", 100},
        {"sessionId", "test-session-seq"},
        {"deviceId", "test-device-seq"},
        {"timestamp", timestamp},
        {"command", static_cast<uint16_t>(CommandType::PING)},
        {"nonce", "nonce-seq-1"}
    };
    
    ValidationResult result = sm.validateProtocolMessage(msg1.dump());
    TEST_ASSERT(result.valid, "First message (id=100) should pass");
    
    // Message with higher ID - should pass
    json msg2 = msg1;
    msg2["messageId"] = 101;
    msg2["nonce"] = "nonce-seq-2";
    result = sm.validateProtocolMessage(msg2.dump());
    TEST_ASSERT(result.valid, "Message with higher ID should pass");
    
    // Message with same ID - should fail (not strictly increasing)
    json msg3 = msg1;
    msg3["messageId"] = 101;
    msg3["nonce"] = "nonce-seq-3";
    result = sm.validateProtocolMessage(msg3.dump());
    TEST_ASSERT(!result.valid, "Message with same ID should fail");
    TEST_ASSERT(result.errorMessage.find("Invalid message sequence") != std::string::npos, "Error should mention sequence");
    
    // Message with lower ID - should fail
    json msg4 = msg1;
    msg4["messageId"] = 99;
    msg4["nonce"] = "nonce-seq-4";
    result = sm.validateProtocolMessage(msg4.dump());
    TEST_ASSERT(!result.valid, "Message with lower ID should fail");
    
    return true;
}

bool testRateLimiting() {
    std::cout << "\n=== Testing Rate Limiting ===" << std::endl;
    
    SecurityManager sm;
    sm.generateSessionToken("test-session-rate", "test-device-rate", std::chrono::hours(1));
    
    // Set aggressive rate limit for testing (no burst allowance)
    RateLimitConfig config;
    config.maxMessagesPerSecond = 5;
    config.maxPayloadBytesPerSecond = 1024;
    config.burstAllowance = 0;
    sm.setRateLimitConfig(config);
    
    int64_t timestamp = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    
    // Send messages up to limit (5 messages)
    for (int i = 0; i < 5; ++i) {
        json msg = {
            {"protocolVersion", 1},
            {"messageId", static_cast<uint64_t>(i + 1)},
            {"sessionId", "test-session-rate"},
            {"deviceId", "test-device-rate"},
            {"timestamp", timestamp},
            {"command", static_cast<uint16_t>(CommandType::PING)},
            {"nonce", "nonce-rate-" + std::to_string(i)}
        };
        
        ValidationResult result = sm.validateProtocolMessage(msg.dump());
        TEST_ASSERT(result.valid, "Message " + std::to_string(i) + " should pass (within limit)");
    }
    
    // Next message should fail (rate limited)
    json msgLimited = {
        {"protocolVersion", 1},
        {"messageId", 10},
        {"sessionId", "test-session-rate"},
        {"deviceId", "test-device-rate"},
        {"timestamp", timestamp},
        {"command", static_cast<uint16_t>(CommandType::PING)},
        {"nonce", "nonce-rate-limited"}
    };
    
    ValidationResult result = sm.validateProtocolMessage(msgLimited.dump());
    TEST_ASSERT(!result.valid, "Message exceeding rate limit should fail");
    TEST_ASSERT(result.errorMessage.find("Rate limit exceeded") != std::string::npos, "Error should mention rate limit");
    
    return true;
}

bool testSessionOwnership() {
    std::cout << "\n=== Testing Session Ownership ===" << std::endl;
    
    SecurityManager sm;
    
    // Create session for device-1
    auto tokenOpt = sm.generateSessionToken("test-session-owner", "device-1", std::chrono::hours(1));
    TEST_ASSERT(tokenOpt.has_value(), "Token generation should succeed");
    
    int64_t timestamp = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    
    // Valid device accessing session
    json msgValid = {
        {"protocolVersion", 1},
        {"messageId", 1},
        {"sessionId", "test-session-owner"},
        {"deviceId", "device-1"},
        {"timestamp", timestamp},
        {"command", static_cast<uint16_t>(CommandType::PING)},
        {"nonce", "nonce-owner-1"}
    };
    
    ValidationResult result = sm.validateProtocolMessage(msgValid.dump());
    TEST_ASSERT(result.valid, "Correct device should access session");
    
    // Different device trying to access session
    json msgInvalid = msgValid;
    msgInvalid["messageId"] = 2;
    msgInvalid["deviceId"] = "device-2";  // Different device
    msgInvalid["nonce"] = "nonce-owner-2";
    result = sm.validateProtocolMessage(msgInvalid.dump());
    TEST_ASSERT(!result.valid, "Different device should not access session");
    TEST_ASSERT(result.errorMessage.find("Device does not own session") != std::string::npos, "Error should mention ownership");
    
    // Non-existent session
    json msgNoSession = msgValid;
    msgNoSession["messageId"] = 3;
    msgNoSession["sessionId"] = "non-existent-session";
    msgNoSession["nonce"] = "nonce-owner-3";
    result = sm.validateProtocolMessage(msgNoSession.dump());
    TEST_ASSERT(!result.valid, "Non-existent session should fail");
    TEST_ASSERT(result.errorMessage.find("Session not found") != std::string::npos, "Error should mention session not found");
    
    return true;
}

bool testTimestampValidation() {
    std::cout << "\n=== Testing Timestamp Validation ===" << std::endl;
    
    SecurityManager sm;
    sm.generateSessionToken("test-session-time", "test-device-time", std::chrono::hours(1));
    
    // Current timestamp - should pass
    int64_t now = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    
    json msgNow = {
        {"protocolVersion", 1},
        {"messageId", 1},
        {"sessionId", "test-session-time"},
        {"deviceId", "test-device-time"},
        {"timestamp", now},
        {"command", static_cast<uint16_t>(CommandType::PING)},
        {"nonce", "nonce-time-1"}
    };
    
    ValidationResult result = sm.validateProtocolMessage(msgNow.dump());
    TEST_ASSERT(result.valid, "Current timestamp should pass");
    
    // Old timestamp (outside 30 second window) - should fail
    json msgOld = msgNow;
    msgOld["messageId"] = 2;
    msgOld["nonce"] = "nonce-time-2";
    msgOld["timestamp"] = now - 60000;  // 60 seconds ago
    result = sm.validateProtocolMessage(msgOld.dump());
    TEST_ASSERT(!result.valid, "Old timestamp should fail");
    TEST_ASSERT(result.errorMessage.find("Timestamp outside acceptable window") != std::string::npos, "Error should mention timestamp");
    
    // Future timestamp (outside 30 second window) - should fail
    json msgFuture = msgNow;
    msgFuture["messageId"] = 3;
    msgFuture["nonce"] = "nonce-time-3";
    msgFuture["timestamp"] = now + 60000;  // 60 seconds in future
    result = sm.validateProtocolMessage(msgFuture.dump());
    TEST_ASSERT(!result.valid, "Future timestamp should fail");
    
    return true;
}

bool testPayloadSizeLimits() {
    std::cout << "\n=== Testing Payload Size Limits ===" << std::endl;
    
    SecurityManager sm;
    sm.generateSessionToken("test-session-size", "test-device-size", std::chrono::hours(1));
    
    int64_t timestamp = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    
    // Create oversized payload for a command with 1KB limit
    std::string largePayload(2000, 'x');  // 2KB > 1KB default limit
    
    json msg = {
        {"protocolVersion", 1},
        {"messageId", 1},
        {"sessionId", "test-session-size"},
        {"deviceId", "test-device-size"},
        {"timestamp", timestamp},
        {"command", static_cast<uint16_t>(CommandType::PING)},
        {"payload", largePayload},  // Invalid: not JSON object for PING
        {"nonce", "nonce-size-1"}
    };
    
    ValidationResult result = sm.validateProtocolMessage(msg.dump());
    TEST_ASSERT(!result.valid, "Oversized/invalid payload should fail");
    
    // Test large valid payload for SESSION_CREATE (8KB limit)
    json largeConfig = {
        {"deviceId", "test-device-size"},
        {"capabilities", json::array()},
        {"extraData", std::string(5000, 'y')}  // 5KB < 8KB limit
    };
    
    json msg2 = {
        {"protocolVersion", 1},
        {"messageId", 2},
        {"sessionId", "test-session-size"},
        {"deviceId", "test-device-size"},
        {"timestamp", timestamp},
        {"command", static_cast<uint16_t>(CommandType::SESSION_CREATE)},
        {"payload", largeConfig},
        {"nonce", "nonce-size-2"}
    };
    
    result = sm.validateProtocolMessage(msg2.dump());
    TEST_ASSERT(result.valid, "Large payload within limit should pass");
    
    return true;
}

bool testSignatureVerification() {
    std::cout << "\n=== Testing Message Signature Verification ===" << std::endl;
    
    SecurityManager sm;
    auto tokenOpt = sm.generateSessionToken("test-session-sig", "test-device-sig", std::chrono::hours(1));
    TEST_ASSERT(tokenOpt.has_value(), "Token generation should succeed");
    
    SessionToken token = tokenOpt.value();
    
    int64_t timestamp = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    
    // Create message and sign it - use json.dump() for consistent payload format
    json payload = {
        {"resolution", "1920x1080"},
        {"fps", 30},
        {"bitrate", 5000000}
    };
    
    ProtocolMessage msg;
    msg.version = ProtocolVersion::V1;
    msg.messageId = 1;
    msg.sessionId = "test-session-sig";
    msg.deviceId = "test-device-sig";
    msg.timestamp = timestamp;
    msg.command = CommandType::VIDEO_CONFIG;
    msg.payload = payload.dump();  // Use JSON dump for consistent format
    msg.nonce = "nonce-sig-1";
    
    // Sign the message
    std::string signature = sm.signMessage(msg, token.hmacKey);
    msg.signature = signature;
    
    // Verify the signature
    bool verified = sm.verifyMessageSignature(msg, token.hmacKey);
    TEST_ASSERT(verified, "Valid signature should verify");
    
    // Tamper with message - should fail
    json tamperedPayload = payload;
    tamperedPayload["fps"] = 60;  // Changed fps
    msg.payload = tamperedPayload.dump();
    verified = sm.verifyMessageSignature(msg, token.hmacKey);
    TEST_ASSERT(!verified, "Tampered message should fail verification");
    
    // Verify through full protocol validation - need to recreate message with same content used for signing
    // The signature was created with messageId=1, so we must use messageId=1 for verification
    json fullMsg = {
        {"protocolVersion", 1},
        {"messageId", 1},  // Must match the signed message
        {"sessionId", "test-session-sig"},
        {"deviceId", "test-device-sig"},
        {"timestamp", timestamp},
        {"command", static_cast<uint16_t>(CommandType::VIDEO_CONFIG)},
        {"payload", payload},  // Use same payload object
        {"signature", signature},
        {"nonce", "nonce-sig-1"}  // Must match the signed message
    };
    
    ValidationResult result = sm.validateProtocolMessage(fullMsg.dump());
    TEST_ASSERT(result.valid, "Full protocol validation with signature should pass");
    
    // Test with wrong signature (same message, different signature)
    // Note: need new messageId and nonce because we already used messageId=1 and nonce-sig-1
    json fullMsg2 = {
        {"protocolVersion", 1},
        {"messageId", 2},  // New message ID
        {"sessionId", "test-session-sig"},
        {"deviceId", "test-device-sig"},
        {"timestamp", timestamp},
        {"command", static_cast<uint16_t>(CommandType::VIDEO_CONFIG)},
        {"payload", payload},  // Same payload
        {"signature", std::string(64, '0')},  // Invalid signature
        {"nonce", "nonce-sig-2"}  // New nonce
    };
    result = sm.validateProtocolMessage(fullMsg2.dump());
    TEST_ASSERT(!result.valid, "Wrong signature should fail");
    TEST_ASSERT(result.errorMessage.find("signature verification failed") != std::string::npos, "Error should mention signature");
    
    return true;
}

bool testConcurrency() {
    std::cout << "\n=== Testing Thread Safety (Concurrency) ===" << std::endl;
    
    SecurityManager sm;
    
    // Disable rate limiting for this test
    RateLimitConfig config;
    config.maxMessagesPerSecond = 10000;
    config.maxPayloadBytesPerSecond = 10000000;
    config.burstAllowance = 10000;
    sm.setRateLimitConfig(config);
    
    int64_t timestamp = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    
    const int numThreads = 4;
    const int messagesPerThread = 25;
    std::atomic<int> successCount{0};
    std::atomic<int> failCount{0};
    
    // Use different sessions per thread to avoid sequence validation conflicts
    auto worker = [&](int threadId) {
        // Each thread gets its own session
        std::string sessionId = "test-session-concurrent-" + std::to_string(threadId);
        std::string deviceId = "test-device-concurrent-" + std::to_string(threadId);
        
        // Create session for this thread
        auto tokenOpt = sm.generateSessionToken(sessionId, deviceId, std::chrono::hours(1));
        if (!tokenOpt.has_value()) {
            failCount += messagesPerThread;
            return;
        }
        
        for (int i = 0; i < messagesPerThread; ++i) {
            json msg = {
                {"protocolVersion", 1},
                {"messageId", static_cast<uint64_t>(i + 1)},  // Sequential per session
                {"sessionId", sessionId},
                {"deviceId", deviceId},
                {"timestamp", timestamp},
                {"command", static_cast<uint16_t>(CommandType::PING)},
                {"nonce", "nonce-concurrent-" + std::to_string(threadId) + "-" + std::to_string(i)}
            };
            
            ValidationResult result = sm.validateProtocolMessage(msg.dump());
            if (result.valid) {
                successCount++;
            } else {
                failCount++;
            }
        }
    };
    
    std::vector<std::thread> threads;
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back(worker, i);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    std::cout << "Concurrent test: " << successCount << " passed, " << failCount << " failed" << std::endl;
    TEST_ASSERT(successCount > 0, "Some messages should pass");
    TEST_ASSERT(failCount == 0, "All messages should pass (rate limiting disabled, no sequence conflicts)");
    
    return true;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "PhoneCam SecurityManager Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    int passed = 0;
    int failed = 0;
    
    auto runTest = [&](const char* name, bool (*testFunc)()) {
        try {
            if (testFunc()) {
                std::cout << "\n*** " << name << " PASSED ***" << std::endl;
                passed++;
            } else {
                std::cout << "\n*** " << name << " FAILED ***" << std::endl;
                failed++;
            }
        } catch (const std::exception& e) {
            std::cerr << "\n*** " << name << " EXCEPTION: " << e.what() << " ***" << std::endl;
            failed++;
        } catch (...) {
            std::cerr << "\n*** " << name << " UNKNOWN EXCEPTION ***" << std::endl;
            failed++;
        }
    };
    
    runTest("Master Key Persistence", testMasterKeyPersistence);
    runTest("JSON Parsing", testJSONParsing);
    runTest("JSON Schema Validation", testJSONSchemaValidation);
    runTest("Replay Protection", testReplayProtection);
    runTest("Message Sequence", testMessageSequence);
    runTest("Rate Limiting", testRateLimiting);
    runTest("Session Ownership", testSessionOwnership);
    runTest("Timestamp Validation", testTimestampValidation);
    runTest("Payload Size Limits", testPayloadSizeLimits);
    runTest("Signature Verification", testSignatureVerification);
    runTest("Concurrency", testConcurrency);
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return failed == 0 ? 0 : 1;
}