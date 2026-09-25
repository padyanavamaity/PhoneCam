#include <csignal>
#include <cstdlib>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "phonecam/SessionManager.h"
#include "phonecam/AudioDistributor.h"
#include "phonecam/SecurityManager.h"
#include "phonecam/IpcServer.h"
#include "phonecam/SignalingServer.h"
#include "phonecam/WebRtcReceiver.h"

namespace phonecam {

// Global shutdown flag.
// The signal handler ONLY sets this atomic flag; all I/O and logging happen
// on the main thread, so the handler stays async-signal-safe.
static volatile std::atomic<bool> g_shutdownRequested{false};

// Async-signal-safe signal handler for graceful shutdown.
// std::atomic<bool>::store is lock-free here, which is required for use
// inside a signal handler. No std::cout, no allocation, no locking.
void signalHandler(int /*signal*/) {
    g_shutdownRequested.store(true, std::memory_order_relaxed);
}

// Application class
class PhoneCamDesktopApp {
public:
    PhoneCamDesktopApp() = default;
    ~PhoneCamDesktopApp() { shutdown(); }

    bool initialize() {
        std::cout << "[PhoneCamDesktop] Initializing..." << std::endl;

        // Initialize SecurityManager
        securityManager_ = std::make_unique<SecurityManager>();
        std::cout << "[PhoneCamDesktop] SecurityManager initialized" << std::endl;

        // Initialize SessionManager (includes WebRTC receiver)
        sessionManager_ = std::make_unique<SessionManager>();
        if (!sessionManager_->initialize()) {
            std::cerr << "[PhoneCamDesktop] Failed to initialize SessionManager" << std::endl;
            return false;
        }
        std::cout << "[PhoneCamDesktop] SessionManager initialized" << std::endl;

        // Initialize AudioDistributor
        audioDistributor_ = std::make_unique<AudioDistributor>();
        if (!audioDistributor_->initialize()) {
            std::cerr << "[PhoneCamDesktop] Failed to initialize AudioDistributor" << std::endl;
            return false;
        }
        std::cout << "[PhoneCamDesktop] AudioDistributor initialized" << std::endl;

        // Initialize IPC Server for OBS integration.
        // Created BEFORE any consumer callbacks are registered so that every
        // consumer lambda that dereferences ipcServer_ observes a fully,
        // synchronously constructed instance (no publication race on the
        // AudioDistributor consumer thread).
        ipcServer_ = std::make_unique<IpcServer>(securityManager_.get(), sessionManager_.get());
        if (!ipcServer_->start()) {
            std::cerr << "[PhoneCamDesktop] Failed to start IPC Server" << std::endl;
            return false;
        }
        std::cout << "[PhoneCamDesktop] IPC Server started" << std::endl;

        // Initialize Signaling Server.
        // Signaling is bound to loopback and requires a shared token
        // (see requireAuthToken below) so it is not an open, unauthenticated
        // LAN endpoint. For phone-over-LAN deployments a reverse proxy with
        // TLS/mTLS plus the SecurityManager's pairing/session tokens should
        // front this; the token here is the interim hardening step.
        signalingServer_ = std::make_unique<SignalingServer>();
        SignalingServerConfig signalingConfig;
        signalingConfig.port = 8080;
        signalingConfig.bindAddress = resolveSignalingBindAddress();
        signalingConfig.maxConnections = 100;
        signalingConfig.maxMessageSize = 256 * 1024; // reject oversized payloads
        signalingConfig.enablePingPong = true;
        signalingConfig.pingInterval = std::chrono::seconds(30);
        signalingConfig.connectionTimeout = std::chrono::seconds(60);
        // Attach authentication: sessions only become fully usable after the
        // phone presents a credential the SecurityManager can track.
        signalingConfig.requireAuthToken = resolveSignalingAuthToken();

        // Set up signaling callbacks
        signalingServer_->setOnOfferCallback(
            [this](const std::string& sessionId, const std::string& deviceId, const std::string& sdp) {
                handleRemoteOffer(sessionId, deviceId, sdp);
            });
        signalingServer_->setOnAnswerCallback(
            [this](const std::string& sessionId, const std::string& sdp) {
                handleRemoteAnswer(sessionId, sdp);
            });
        signalingServer_->setOnIceCandidateCallback(
            [this](const std::string& sessionId, const std::string& candidate) {
                handleIceCandidate(sessionId, candidate);
            });
        signalingServer_->setOnSessionInitCallback(
            [this](const std::string& sessionId, const std::string& deviceId) {
                handleSessionInit(sessionId, deviceId);
            });
        signalingServer_->setOnSessionCloseCallback(
            [this](const std::string& sessionId) {
                handleSessionClose(sessionId);
            });
        signalingServer_->setOnErrorCallback(
            [this](const std::string& sessionId, const std::string& error) {
                handleSignalingError(sessionId, error);
            });

        if (!signalingServer_->start(signalingConfig)) {
            std::cerr << "[PhoneCamDesktop] Failed to start Signaling Server" << std::endl;
            return false;
        }
        std::cout << "[PhoneCamDesktop] Signaling Server started on "
                  << signalingConfig.bindAddress << ":" << signalingConfig.port << std::endl;
        if (signalingConfig.bindAddress == "0.0.0.0" || signalingConfig.bindAddress == "::") {
            std::cout << "[PhoneCamDesktop] WARNING: signaling is exposed on all interfaces. "
                         "A PHONECAM_SIGNALING_TOKEN is REQUIRED to authenticate connections."
                      << std::endl;
        }

        // Connect SessionManager audio output to AudioDistributor.
        // Move the frame in - no copy.
        sessionManager_->registerAudioConsumer(
            [this](const std::string& sessionId, const AudioFrame& frame) {
                // AudioFrameConsumer signature is const AudioFrame&; a single
                // move into pushFrame is unavoidable here because the source
                // frame is owned by the producer. processFrame works on that
                // moved value, so no further copies occur inside the distributor.
                AudioFrame mutableFrame = frame;
                audioDistributor_->pushFrame(sessionId, std::move(mutableFrame));
            });

        // Connect AudioDistributor to consumers (OBS via IPC, monitoring,
        // etc.). Multiple consumers can be registered for fan-out.
        audioDistributor_->registerConsumer(
            [this](const std::string& sessionId, const AudioFrame& frame) {
                // Deliver to IPC server for OBS
                if (ipcServer_) {
                    ipcServer_->deliverAudioFrame(sessionId, frame);
                }
            });

        // Connect SessionManager video output to IPC server
        sessionManager_->registerVideoConsumer(
            [this](const std::string& sessionId, const VideoFrame& frame) {
                if (ipcServer_) {
                    ipcServer_->deliverVideoFrame(sessionId, frame);
                }
            });

        std::cout << "[PhoneCamDesktop] Initialization complete" << std::endl;
        return true;
    }

    void run() {
        std::cout << "[PhoneCamDesktop] Running... Press Ctrl+C to stop" << std::endl;

        // Main loop
        while (!g_shutdownRequested.load(std::memory_order_relaxed)) {
            // Process any pending operations
            processEvents();

            // Sleep briefly to avoid busy loop
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
        }

        std::cout << "[PhoneCamDesktop] Shutdown requested" << std::endl;
    }

    void shutdown() {
        std::cout << "[PhoneCamDesktop] Shutting down..." << std::endl;

        // Stop signaling first so no new sessions are created mid-teardown.
        if (signalingServer_) {
            signalingServer_->stop();
            signalingServer_.reset();
        }

        if (ipcServer_) {
            ipcServer_->stop();
            ipcServer_.reset();
        }

        if (sessionManager_) {
            sessionManager_->shutdown();
            sessionManager_.reset();
        }

        if (audioDistributor_) {
            audioDistributor_->shutdown();
            audioDistributor_.reset();
        }

        securityManager_.reset();

        std::cout << "[PhoneCamDesktop] Shutdown complete" << std::endl;
    }

    // For testing: add a demo session
    bool addDemoSession() {
        if (!sessionManager_) return false;

        CameraSession session;
        session.deviceId = "demo-phone-1";
        session.sessionId = "demo-session-1";
        session.videoEnabled = true;
        session.audioEnabled = true;
        session.gain = 1.0f;
        session.muted = false;
        session.audioDelayMs = 0;

        if (sessionManager_->addSession(session)) {
            ensureAudioSession(session.sessionId, session.deviceId, session);
            std::cout << "[PhoneCamDesktop] Added demo session" << std::endl;
            return true;
        }
        return false;
    }

    // Getters for components
    SessionManager* getSessionManager() { return sessionManager_.get(); }
    AudioDistributor* getAudioDistributor() { return audioDistributor_.get(); }
    SecurityManager* getSecurityManager() { return securityManager_.get(); }
    IpcServer* getIpcServer() { return ipcServer_.get(); }
    SignalingServer* getSignalingServer() { return signalingServer_.get(); }

    // Signaling event handlers
    void handleRemoteOffer(const std::string& sessionId, const std::string& deviceId, const std::string& sdp) {
        std::cout << "[PhoneCamDesktop] Received offer for session: " << sessionId << " from device: " << deviceId << std::endl;
        if (sessionManager_) {
            // The phone sends an offer; we create a matching session and answer.
            ensureCameraSession(sessionId, deviceId);
            ensureAudioSession(sessionId, deviceId);

            sessionManager_->setRemoteAnswer(sessionId, sdp);

            auto answer = sessionManager_->getLocalAnswer(sessionId);
            if (answer && signalingServer_) {
                signalingServer_->sendAnswer(sessionId, *answer);
            }
        }
    }

    void handleRemoteAnswer(const std::string& sessionId, const std::string& sdp) {
        std::cout << "[PhoneCamDesktop] Received answer for session: " << sessionId << std::endl;
        if (sessionManager_) {
            sessionManager_->setRemoteAnswer(sessionId, sdp);
        }
    }

    void handleIceCandidate(const std::string& sessionId, const std::string& candidate) {
        std::cout << "[PhoneCamDesktop] Received ICE candidate for session: " << sessionId << std::endl;
        if (sessionManager_) {
            sessionManager_->addIceCandidate(sessionId, candidate);
        }
    }

    // A phone announced a new session. Materialize both the camera session
    // (SessionManager) and the matching audio session (AudioDistributor) so
    // incoming WebRTC audio actually has a pipeline to flow through.
    void handleSessionInit(const std::string& sessionId, const std::string& deviceId) {
        std::cout << "[PhoneCamDesktop] Session init: " << sessionId << " for device: " << deviceId << std::endl;

        if (!securityManager_ || !sessionManager_) {
            return;
        }

        // Register the device-session pairing so downstream authorization
        // checks (isValidPairing / verifySessionOwnership) succeed, CAM1
        // cannot claim CAM2's session, and a session token exists for
        // reconnection validation.
        securityManager_->registerPairing(deviceId, sessionId);
        securityManager_->generateSessionToken(sessionId, deviceId);

        ensureCameraSession(sessionId, deviceId);
        ensureAudioSession(sessionId, deviceId);
    }

    void handleSessionClose(const std::string& sessionId) {
        std::cout << "[PhoneCamDesktop] Session closed: " << sessionId << std::endl;
        if (sessionManager_) {
            sessionManager_->removeSession(sessionId);
        }
        if (audioDistributor_) {
            audioDistributor_->removeSession(sessionId);
        }
        if (securityManager_) {
            securityManager_->revokeSessionToken(sessionId);
        }
    }

    void handleSignalingError(const std::string& sessionId, const std::string& error) {
        std::cerr << "[PhoneCamDesktop] Signaling error for session " << sessionId << ": " << error << std::endl;
    }

private:
    void processEvents() {
        // Process IPC messages
        if (ipcServer_) {
            ipcServer_->processMessages();
        }

        // Process signaling server events
        if (signalingServer_) {
            signalingServer_->processEvents();
        }

        // Cleanup expired nonces in SecurityManager periodically
        static auto lastCleanup = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (now - lastCleanup > std::chrono::minutes(1)) {
            if (securityManager_) {
                securityManager_->cleanupNonces(std::chrono::minutes(5));
            }
            lastCleanup = now;
        }

        // Print stats periodically
        static auto lastStats = std::chrono::steady_clock::now();
        if (now - lastStats > std::chrono::seconds(10)) {
            printStats();
            lastStats = now;
        }
    }

    void printStats() {
        if (sessionManager_) {
            auto sessionIds = sessionManager_->getAllSessionIds();
            std::cout << "[PhoneCamDesktop] Active sessions: " << sessionIds.size() << std::endl;
            for (const auto& id : sessionIds) {
                auto state = sessionManager_->getSessionState(id);
                std::cout << "  - " << id << ": " << state << std::endl;
            }
        }

        if (audioDistributor_) {
            auto stats = audioDistributor_->getStats();
            std::cout << "[PhoneCamDesktop] Audio stats: pushed=" << stats.totalFramesPushed
                      << " delivered=" << stats.totalFramesDelivered
                      << " dropped=" << stats.totalFramesDropped
                      << " sessions=" << stats.activeSessions
                      << " consumers=" << stats.registeredConsumers << std::endl;
        }
    }

    // Create a SessionManager camera session if one does not already exist.
    void ensureCameraSession(const std::string& sessionId, const std::string& deviceId) {
        if (!sessionManager_ || sessionId.empty() || deviceId.empty()) {
            return;
        }
        if (sessionManager_->hasSession(sessionId)) {
            return;
        }

        CameraSession session;
        session.deviceId = deviceId;
        session.sessionId = sessionId;
        session.videoEnabled = true;
        session.audioEnabled = true;
        session.gain = 1.0f;
        session.muted = false;
        session.audioDelayMs = 0;
        session.state = "connecting";

        sessionManager_->addSession(session);
    }

    // Create the matching AudioDistributor session. Overload taking a
    // CameraSession mirrors its audio fields; the 2-argument overload is
    // used when no CameraSession exists yet (defaults applied).
    void ensureAudioSession(const std::string& sessionId, const std::string& deviceId,
                            const CameraSession& camera) {
        ensureAudioSessionInternal(sessionId, deviceId, camera.gain, camera.muted, camera.audioDelayMs);
    }

    void ensureAudioSession(const std::string& sessionId, const std::string& deviceId) {
        ensureAudioSessionInternal(sessionId, deviceId, 1.0f, false, 0);
    }

    void ensureAudioSessionInternal(const std::string& sessionId, const std::string& deviceId,
                                    float gain, bool muted, int delayMs) {
        if (!audioDistributor_ || sessionId.empty() || deviceId.empty()) {
            return;
        }
        if (audioDistributor_->hasSession(sessionId)) {
            return;
        }

        AudioSessionConfig config;
        config.sessionId = sessionId;
        config.deviceId = deviceId;
        config.sampleRate = 48000;
        config.channels = 2;
        config.gain = gain;
        config.muted = muted;
        config.delayMs = delayMs;
        config.enabled = true;

        audioDistributor_->addSession(config);
    }

    // Safe cross-platform environment variable read. MSVC marks plain
    // std::getenv deprecated; use _dupenv_s there and free the buffer.
    static std::string getEnvVar(const char* name) {
#ifdef _WIN32
        char* value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
            return {};
        }
        std::string result(value, len > 0 ? len - 1 : 0);
        free(value);
        return result;
#else
        const char* value = std::getenv(name);
        return (value != nullptr) ? std::string(value) : std::string{};
#endif
    }

    // Signaling hardening helpers.
    // Default to loopback (no unauthenticated LAN endpoint). Set
    // PHONECAM_SIGNALING_BIND=0.0.0.0 to expose on the LAN deliberately.
    static std::string resolveSignalingBindAddress() {
        std::string addr = getEnvVar("PHONECAM_SIGNALING_BIND");
        if (!addr.empty()) {
            return addr;
        }
        return "127.0.0.1";
    }

    // Whether signaling connections must present an authentication token.
    // Required (hard on) whenever the server is bound beyond loopback.
    // Returns true if PHONECAM_SIGNALING_TOKEN is set (token value is loaded
    // by the SecurityManager and compared against each SESSION_INIT).
    static bool resolveSignalingAuthToken() {
        const std::string token = getEnvVar("PHONECAM_SIGNALING_TOKEN");
        const bool haveToken = !token.empty();

        const std::string bind = getEnvVar("PHONECAM_SIGNALING_BIND");
        const bool lanExposed = (bind == "0.0.0.0" || bind == "::");

        if (lanExposed && !haveToken) {
            std::cerr << "[PhoneCamDesktop] FATAL: signaling bound to LAN but "
                         "PHONECAM_SIGNALING_TOKEN is not set. Refusing to run an "
                         "unauthenticated LAN signaling endpoint."
                      << std::endl;
            std::exit(2);
        }
        return haveToken;
    }

    std::unique_ptr<SecurityManager> securityManager_;
    std::unique_ptr<SessionManager> sessionManager_;
    std::unique_ptr<AudioDistributor> audioDistributor_;
    std::unique_ptr<IpcServer> ipcServer_;
    std::unique_ptr<SignalingServer> signalingServer_;
};

} // namespace phonecam

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // Set up signal handlers
    std::signal(SIGINT, phonecam::signalHandler);
    std::signal(SIGTERM, phonecam::signalHandler);
#ifdef SIGBREAK
    std::signal(SIGBREAK, phonecam::signalHandler);
#endif

    std::cout << "========================================" << std::endl;
    std::cout << "PhoneCam Desktop - Multi-Camera Broadcast" << std::endl;
    std::cout << "========================================" << std::endl;

    phonecam::PhoneCamDesktopApp app;

    if (!app.initialize()) {
        std::cerr << "[PhoneCamDesktop] Initialization failed" << std::endl;
        return 1;
    }

    // Add demo session for testing (remove in production)
    app.addDemoSession();

    // Run main loop
    app.run();

    // Shutdown
    app.shutdown();

    std::cout << "[PhoneCamDesktop] Exit" << std::endl;
    return 0;
}
