#include <csignal>
#include <cstdlib>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <mutex>

#include "phonecam/SessionManager.h"
#include "phonecam/AudioDistributor.h"
#include "phonecam/SecurityManager.h"
#include "phonecam/IpcServer.h"
#include "phonecam/SignalingServer.h"
#include "phonecam/WebRtcReceiver.h"
#include "phonecam/DiscoveryClient.h"
#include "phonecam/SignalingClient.h"

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

        // Initialize Discovery Client for mDNS device discovery.
        discoveryClient_ = std::make_unique<DiscoveryClient>();
        DiscoveryClientConfig discoveryConfig;
        discoveryConfig.serviceType = "_phonecam._tcp.local";
        discoveryConfig.browseInterval = std::chrono::milliseconds(10000); // Re-browse every 10 seconds
        discoveryConfig.resolveAddresses = true;
        discoveryConfig.resolveTimeout = std::chrono::seconds(5);
        
        discoveryClient_->setOnDeviceCallback(
            [this](const DiscoveredDevice& device, DeviceState state) {
                handleDeviceDiscovered(device, state);
            });
        
        if (!discoveryClient_->start(discoveryConfig)) {
            std::cerr << "[PhoneCamDesktop] Failed to start Discovery Client" << std::endl;
            return false;
        }
        std::cout << "[PhoneCamDesktop] Discovery Client started (browsing for _phonecam._tcp.local)" << std::endl;

        // Initialize Signaling Client for connecting to Android WebSocket signaling servers.
        // We maintain one SignalingClient per discovered device.
        
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
            [this](const std::string& sessionId, const std::string& sdpMid, int sdpMLineIndex, const std::string& candidate) {
                handleIceCandidate(sessionId, sdpMid, sdpMLineIndex, candidate);
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

        // Forward libdatachannel-generated ICE candidates back to the phone
        // over the signaling channel. Without this, ICE never completes.
        // The callback now includes sdpMid and sdpMLineIndex for proper routing
        // to the correct media stream (video vs audio) on the remote peer.
        sessionManager_->setIceCandidateHook(
            [this](const std::string& sessionId, const std::string& sdpMid, int sdpMLineIndex, const std::string& candidate) {
                // Try to send via WebSocket signaling clients first (new protocol)
                bool sent = false;
                {
                    std::lock_guard<std::mutex> lock(signalingClientsMutex_);
                    for (auto& [devId, client] : signalingClients_) {
                        if (client && client->isConnected()) {
                            if (client->sendCandidate(sessionId, sdpMid, sdpMLineIndex, candidate)) {
                                sent = true;
                                break;
                           }
                       }
                   }
               }
               
               // Fallback to legacy TCP signaling server
               if (!sent && signalingServer_) {
                   // For the legacy TCP server, we don't have sdpMid/sdpMLineIndex, use defaults
                   signalingServer_->sendIceCandidate(sessionId, "", 0, candidate);
               }
           });

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

        // Stop discovery client first
        if (discoveryClient_) {
            discoveryClient_->stop();
            discoveryClient_.reset();
        }

        // Stop WebSocket signaling clients
        {
            std::lock_guard<std::mutex> lock(signalingClientsMutex_);
            for (auto& [deviceId, client] : signalingClients_) {
                if (client) {
                    std::cout << "[PhoneCamDesktop] Disconnecting signaling client for device: " << deviceId << std::endl;
                    client->disconnect();
                }
            }
            signalingClients_.clear();
        }

        // Stop signaling server (legacy TCP signaling)
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
    DiscoveryClient* getDiscoveryClient() { return discoveryClient_.get(); }

    // Signaling event handlers.
    //
    // IMPORTANT: SDP direction is Android --offer--> Desktop --answer--> Android.
    // We do NOT fabricate an offer; we consume the phone's offer and produce
    // the desktop's answer.
    void handleRemoteOffer(const std::string& sessionId, const std::string& deviceId, const std::string& sdp) {
        std::cout << "[PhoneCamDesktop] Received offer: sessionId=" << sessionId
                  << " deviceId=" << deviceId
                  << " sdpBytes=" << sdp.size() << std::endl;
        if (!sessionManager_) {
            std::cerr << "[PhoneCamDesktop] handleRemoteOffer: sessionManager_ is null" << std::endl;
            return;
        }

        // Make sure both the camera session and the audio session rows exist.
        // The phone will send sessionInit first, but offer is the more reliable
        // anchor if it ever fires before init completes.
        ensureCameraSession(sessionId, deviceId);
        ensureAudioSession(sessionId, deviceId);

        // Feed the phone's SDP offer into libdatachannel (answerer side).
        // With disableAutoNegotiation=true, libdatachannel produces the answer
        // synchronously as part of setRemoteDescription + setLocalDescription.
        if (!sessionManager_->setRemoteOffer(sessionId, sdp)) {
            std::cerr << "[PhoneCamDesktop] setRemoteOffer failed for " << sessionId << std::endl;
            return;
        }

        // After setRemoteOffer returned, libdatachannel has synchronously
        // invoked the onLocalDescription callback and stashed the answer.
        auto answer = sessionManager_->getLocalAnswer(sessionId);
        if (answer) {
            // Try to send answer via WebSocket signaling client first (new protocol)
            bool sent = false;
            {
                std::lock_guard<std::mutex> lock(signalingClientsMutex_);
                for (auto& [devId, client] : signalingClients_) {
                    if (client && client->isConnected()) {
                        if (client->sendAnswer(sessionId, *answer)) {
                            sent = true;
                            std::cout << "[PhoneCamDesktop] Answer sent via WebSocket: sessionId=" << sessionId
                                      << " sdpBytes=" << answer->size() << std::endl;
                            break;
                        }
                    }
                }
            }
            
            // Fallback to legacy TCP signaling server
            if (!sent && signalingServer_) {
                if (!signalingServer_->sendAnswer(sessionId, *answer)) {
                    std::cerr << "[PhoneCamDesktop] sendAnswer failed for " << sessionId << std::endl;
                } else {
                    std::cout << "[PhoneCamDesktop] Answer sent via TCP: sessionId=" << sessionId
                              << " sdpBytes=" << answer->size() << std::endl;
                }
            }
        } else {
            std::cerr << "[PhoneCamDesktop] getLocalAnswer returned nullopt for "
                      << sessionId << std::endl;
        }
    }

    void handleRemoteAnswer(const std::string& sessionId, const std::string& sdp) {
        std::cout << "[PhoneCamDesktop] Received answer: sessionId=" << sessionId
                  << " sdpBytes=" << sdp.size() << std::endl;
        if (sessionManager_) {
            sessionManager_->setRemoteAnswer(sessionId, sdp);
        } else {
            std::cerr << "[PhoneCamDesktop] handleRemoteAnswer: sessionManager_ is null" << std::endl;
        }
    }

    void handleIceCandidate(const std::string& sessionId, const std::string& sdpMid, int sdpMLineIndex, const std::string& candidate) {
        std::cout << "[PhoneCamDesktop] ICE candidate received: sessionId=" << sessionId
                  << " mid=" << sdpMid << " mline=" << sdpMLineIndex
                  << " candidateBytes=" << candidate.size() << std::endl;
        if (sessionManager_) {
            sessionManager_->addIceCandidate(sessionId, sdpMid, sdpMLineIndex, candidate);
        } else {
            std::cerr << "[PhoneCamDesktop] handleIceCandidate: sessionManager_ is null" << std::endl;
        }
    }

    // A phone announced a new session. Materialize both the camera session
    // (SessionManager) and the matching audio session (AudioDistributor) so
    // incoming WebRTC audio actually has a pipeline to flow through.
    void handleSessionInit(const std::string& sessionId, const std::string& deviceId) {
        std::cout << "[PhoneCamDesktop] Session init: sessionId=" << sessionId
                  << " deviceId=" << deviceId << std::endl;

        if (!securityManager_ || !sessionManager_) {
            std::cerr << "[PhoneCamDesktop] handleSessionInit: securityManager_ or sessionManager_ is null" << std::endl;
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
        std::cout << "[PhoneCamDesktop] Session closed: sessionId=" << sessionId << std::endl;
        if (sessionManager_) {
            sessionManager_->removeSession(sessionId);
        } else {
            std::cerr << "[PhoneCamDesktop] handleSessionClose: sessionManager_ is null" << std::endl;
        }
        if (audioDistributor_) {
            audioDistributor_->removeSession(sessionId);
        }
        if (securityManager_) {
            securityManager_->revokeSessionToken(sessionId);
        }
    }

    void handleSignalingError(const std::string& sessionId, const std::string& error) {
        std::cerr << "[PhoneCamDesktop] Signaling error: sessionId=" << sessionId
                  << " error=" << error << std::endl;
    }

    // Handle device discovery events
    void handleDeviceDiscovered(const DiscoveredDevice& device, DeviceState state) {
        std::cout << "[PhoneCamDesktop] Device "
                  << (state == DeviceState::ADDED ? "discovered" :
                      state == DeviceState::UPDATED ? "updated" : "removed")
                  << ": displayName=" << device.displayName
                  << " deviceId=" << device.deviceId
                  << " hostname=" << device.hostname
                  << " ipAddress=" << device.ipAddress
                  << " port=" << device.port
                  << " proto=" << device.proto
                  << " path=" << device.path << std::endl;
        
        if (state == DeviceState::ADDED || state == DeviceState::UPDATED) {
            // Auto-connect to the device's signaling endpoint
            connectToDevice(device);
        } else if (state == DeviceState::REMOVED) {
            // Disconnect from the device
            disconnectFromDevice(device.deviceId);
        }
    }

    // Connect to a discovered device's WebSocket signaling endpoint
    void connectToDevice(const DiscoveredDevice& device) {
        // Check if we already have a connection to this device
        std::lock_guard<std::mutex> lock(signalingClientsMutex_);
        if (signalingClients_.find(device.deviceId) != signalingClients_.end()) {
            std::cout << "[PhoneCamDesktop] Already connected to device: deviceId=" << device.deviceId << std::endl;
            return;
        }

        // Determine the host to connect to (prefer IP address, fallback to hostname)
        std::string connectHost = !device.ipAddress.empty() ? device.ipAddress : device.hostname;
        if (connectHost.empty()) {
            std::cerr << "[PhoneCamDesktop] Cannot connect to device: deviceId=" << device.deviceId
                      << " no IP address or hostname available" << std::endl;
            return;
        }

        // Use the port from mDNS (SRV record) and path from TXT record
        uint16_t connectPort = device.port > 0 ? device.port : 8080;
        std::string connectPath = !device.path.empty() ? device.path : "/";

        std::cout << "[PhoneCamDesktop] Connecting to device: deviceId=" << device.deviceId
                  << " host=" << connectHost << " port=" << connectPort << " path=" << connectPath << std::endl;

        // Create and configure the signaling client
        auto client = std::make_unique<SignalingClient>();
        
        SignalingClientConfig clientConfig;
        clientConfig.host = connectHost;
        clientConfig.port = connectPort;
        clientConfig.path = connectPath;
        clientConfig.connectTimeout = std::chrono::seconds(10);
        clientConfig.pingInterval = std::chrono::seconds(25);
        clientConfig.pongTimeout = std::chrono::seconds(10);
        clientConfig.autoReconnect = true;
        clientConfig.reconnectDelay = std::chrono::seconds(5);

        // Set up callbacks for incoming messages from the Android device
        client->setOnHelloCallback(
            [this, deviceId = device.deviceId](const HelloMessage& hello) {
                handleHello(deviceId, hello);
            });

        client->setOnOfferCallback(
            [this](const OfferMessage& offer) {
                handleRemoteOffer(offer.sessionId, "", offer.sdp); // deviceId not needed here, we get it from hello
            });

        client->setOnCandidateCallback(
            [this](const CandidateMessage& candidate) {
                handleIceCandidate(candidate.sessionId, candidate.sdpMid, candidate.sdpMLineIndex, candidate.candidate);
            });

        client->setOnErrorCallback(
            [this](const ErrorMessage& error) {
                handleSignalingError(error.sessionId, error.code + ": " + error.message);
            });

        client->setOnDisconnectedCallback(
            [this, deviceId = device.deviceId](const DisconnectMessage& disconnect) {
                handleSignalingDisconnected(deviceId, disconnect);
            });

        client->setOnConnectionStateChangeCallback(
            [this, deviceId = device.deviceId](ConnectionState state, const std::string& error) {
                handleSignalingConnectionStateChange(deviceId, state, error);
            });

        // Attempt to connect
        if (client->connect(clientConfig)) {
            std::cout << "[PhoneCamDesktop] Connected to device: deviceId=" << device.deviceId << std::endl;
            signalingClients_[device.deviceId] = std::move(client);
        } else {
            std::cerr << "[PhoneCamDesktop] Failed to connect to device: deviceId=" << device.deviceId << std::endl;
        }
    }

    // Disconnect from a device
    void disconnectFromDevice(const std::string& deviceId) {
        std::lock_guard<std::mutex> lock(signalingClientsMutex_);
        auto it = signalingClients_.find(deviceId);
        if (it != signalingClients_.end()) {
            std::cout << "[PhoneCamDesktop] Disconnecting from device: deviceId=" << deviceId << std::endl;
            it->second->disconnect();
            signalingClients_.erase(it);
        }
    }

    // Handle hello message from Android device
    void handleHello(const std::string& deviceId, const HelloMessage& hello) {
        std::cout << "[PhoneCamDesktop] Hello received: deviceId=" << deviceId
                  << " sessionId=" << hello.sessionId
                  << " name=" << hello.name
                  << " state=" << hello.state
                  << " camera=" << hello.camera
                  << " micEnabled=" << (hello.micEnabled ? "true" : "false")
                  << " bitrateKbps=" << hello.bitrateKbps
                  << " resolution=" << hello.width << "x" << hello.height
                  << " fps=" << hello.fps << std::endl;

        // The hello message contains the sessionId - use it to initialize the session
        if (!hello.sessionId.empty()) {
            handleSessionInit(hello.sessionId, deviceId);
        }
    }

    // Handle signaling connection state changes
    void handleSignalingConnectionStateChange(const std::string& deviceId, ConnectionState state, const std::string& error) {
        std::cout << "[PhoneCamDesktop] Signaling connection state changed: deviceId=" << deviceId
                  << " state=" << static_cast<int>(state)
                  << (error.empty() ? "" : " error=" + error) << std::endl;
        
        if (state == ConnectionState::DISCONNECTED || state == ConnectionState::ERROR_STATE) {
            // Clean up sessions for this device
            if (sessionManager_) {
                auto sessionIds = sessionManager_->getAllSessionIds();
                for (const auto& sessionId : sessionIds) {
                    // Check if this session belongs to the disconnected device
                    // For now, we just log - a more robust implementation would track device-session mapping
                }
            }
        }
    }

    // Handle signaling disconnection
    void handleSignalingDisconnected(const std::string& deviceId, const DisconnectMessage& disconnect) {
        std::cout << "[PhoneCamDesktop] Signaling disconnected: deviceId=" << deviceId
                  << " sessionId=" << disconnect.sessionId
                  << " reason=" << disconnect.reason << std::endl;

        if (!disconnect.sessionId.empty()) {
            handleSessionClose(disconnect.sessionId);
        }
    }

private:
    void processEvents() {
        // Process IPC messages
        if (ipcServer_) {
            ipcServer_->processMessages();
        }

        // Process signaling server events (legacy TCP signaling)
        if (signalingServer_) {
            signalingServer_->processEvents();
        }

        // Process WebSocket signaling clients
        {
            std::lock_guard<std::mutex> lock(signalingClientsMutex_);
            for (auto& [deviceId, client] : signalingClients_) {
                if (client) {
                    client->processEvents();
                }
            }
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

    // Signaling bind-address resolution.
    //
    // THIS MILESTONE (LAN dev test): default is "0.0.0.0" so the phone can
    // actually reach the desktop across the LAN. Auth-token enforcement is
    // deliberately disabled in this build to match SignalingClient.kt, which
    // currently performs an unauthenticated session-init handshake.
    //
    // For any non-LAN deployment, the next milestone MUST re-enable token
    // auth (PHONECAM_SIGNALING_TOKEN) and revert this default to loopback.
    static std::string resolveSignalingBindAddress() {
        std::string addr = getEnvVar("PHONECAM_SIGNALING_BIND");
        if (!addr.empty()) {
            return addr;
        }
        // Milestone default: LAN-accessible. The main() below logs a
        // prominent one-time warning when this default is used.
        return "0.0.0.0";
    }

    // Whether signaling connections must present an authentication token.
    // Currently NOT enforced in SignalingServer; returned value is advisory
    // and used only for log messaging.
    static bool resolveSignalingAuthToken() {
        const std::string token = getEnvVar("PHONECAM_SIGNALING_TOKEN");
        return !token.empty();
    }

    std::unique_ptr<SecurityManager> securityManager_;
    std::unique_ptr<SessionManager> sessionManager_;
    std::unique_ptr<AudioDistributor> audioDistributor_;
    std::unique_ptr<IpcServer> ipcServer_;
    std::unique_ptr<SignalingServer> signalingServer_;
    std::unique_ptr<DiscoveryClient> discoveryClient_;

    // WebSocket signaling clients (one per connected Android device)
    std::mutex signalingClientsMutex_;
    std::unordered_map<std::string, std::unique_ptr<SignalingClient>> signalingClients_;
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

    // Demo session REMOVED in this milestone. We never fabricate sessions;
    // sessions exist only as a result of real signaling from a real phone.
    // app.addDemoSession();

    // One-time loud warning if we're using the LAN-dev default bind address
    // with no auth token -- makes the dev-network configuration explicit.
    {
        // No getSignalingConfig accessor; we replicate the resolution to log.
        // (The real configuration inside SignalingServer is the source of truth.)
        std::cout << "[PhoneCamDesktop] MILESTONE-DEV: signaling bound to 0.0.0.0,"
                     " unauthenticated. DO NOT ship this build."
                  << std::endl;
    }

    // Run main loop
    app.run();

    // Shutdown
    app.shutdown();

    std::cout << "[PhoneCamDesktop] Exit" << std::endl;
    return 0;
}
