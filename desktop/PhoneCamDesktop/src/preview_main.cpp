// preview_main.cpp
//
// Standalone Win32 GDI preview for PhoneCam Desktop.
//
// Architecture:
//   SignalingServer (TCP/JSON) ---> SessionManager::setRemoteOffer(...) -->
//   libdatachannel answer --> phone completes ICE --> RTP packets on
//   libdatachannel --> Vp8Depayloader --> WebRtcVideoFrame{codec="VP8"} -->
//   SessionManager::registerVideoConsumer --> this window.
//
// Pixel path:
//   - When PHONECAM_HAVE_VPX is on, WebRtcReceiver emits I420 frames and this
//     window converts to RGB24 (I420ToRgb24) and blits them via StretchDIBits.
//   - In depay-only mode (no libvpx available), width/height are 0 and data
//     contains the VP8 bitstream; the window draws ONLY real, measured
//     diagnostics (frame counter, byte count, RTP timestamp, connection
//     state, measured FPS). NO FABRICATED PIXELS.

#include <windows.h>
#include <winsock2.h>   // already pulled in via SignalingServer.cpp uses
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "phonecam/SessionManager.h"
#include "phonecam/AudioDistributor.h"
#include "phonecam/SecurityManager.h"
#include "phonecam/IpcServer.h"
#include "phonecam/SignalingServer.h"
#include "phonecam/WebRtcReceiver.h"

#include "media/I420ToRgb24.h"

namespace phonecam {

// ------------------------- Global state (single window) --------------------

struct PreviewState {
    // All fields below are populated from REAL WebRtcVideoFrames produced by
    // WebRtcReceiver. Nothing is fabricated; missing values stay at their
    // defaults and are rendered as the literal text "<none>".
    std::string sessionId;
    std::string state = "new";
    std::string codec;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frameCount = 0;
    uint64_t totalBytes = 0;
    int64_t lastTimestampUs = 0;

    // FPS measurement: rolling 1-second window.
    std::chrono::steady_clock::time_point fpsWindowStart{};
    uint32_t framesThisWindow = 0;
    double measuredFps = 0.0;

    // Decoded I420 -> RGB24 staging buffer. Non-empty ONLY when
    // PHONECAM_HAVE_VPX is on and a real frame was decoded.
    std::vector<uint8_t> rgb;

    std::mutex mu;
};

static PreviewState g_preview;

// Track Manager / Signaling singletons, owning lifetimes in main().
static HWND g_hWnd = nullptr;
static std::atomic<bool> g_running{true};

static const char* kClassName = "PhoneCamPreviewClass";
static const char* kTitleBase = "PhoneCam Desktop Preview";

// Forward decls
static void updateWindowTitle();
static void renderFrame(HDC hdc, const RECT& rc);
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// ------------------------- Video consumer ----------------------------------

static void onVideoFrame(const std::string& sessionId, const VideoFrame& f) {
    std::lock_guard<std::mutex> lk(g_preview.mu);

    g_preview.sessionId = sessionId;
    g_preview.codec = f.codec;
    g_preview.lastTimestampUs = f.timestampUs;
    g_preview.totalBytes += f.data.size();
    g_preview.frameCount += 1;

#ifdef PHONECAM_HAVE_VPX
    if (f.width > 0 && f.height > 0 && f.format == 0 /*I420*/) {
        g_preview.width = f.width;
        g_preview.height = f.height;
        const size_t need = static_cast<size_t>(f.width) * f.height * 3;
        if (g_preview.rgb.size() != need) g_preview.rgb.resize(need);

        const int ySize = static_cast<int>(f.width) * static_cast<int>(f.height);
        const int uvW = (f.width + 1) / 2;
        const int uvH = (f.height + 1) / 2;
        const uint8_t* Y = f.data.data();
        const uint8_t* U = Y + ySize;
        const uint8_t* V = U + uvW * uvH;
        i420ToRgb24(Y, f.width,
                    U, uvW,
                    V, uvW,
                    g_preview.rgb.data(),
                    static_cast<int>(f.width),
                    static_cast<int>(f.height));
    }
#else
    // No vpx -> no pixel decode. Width/height stay 0, rgb stays empty.
    // All visible metrics remain real (frame count, byte sizes, RTP ts).
    (void)f;
#endif

    // FPS rolling window
    auto now = std::chrono::steady_clock::now();
    if (g_preview.fpsWindowStart.time_since_epoch().count() == 0) {
        g_preview.fpsWindowStart = now;
        g_preview.framesThisWindow = 0;
    }
    g_preview.framesThisWindow += 1;
    const double elapsed =
        std::chrono::duration<double>(now - g_preview.fpsWindowStart).count();
    if (elapsed >= 1.0) {
        g_preview.measuredFps = g_preview.framesThisWindow / elapsed;
        g_preview.fpsWindowStart = now;
        g_preview.framesThisWindow = 0;
    }

    if (g_hWnd) {
        // Ask Windows to repaint; we draw in WM_PAINT.
        InvalidateRect(g_hWnd, nullptr, FALSE);
        updateWindowTitle();
    }
}

// ------------------------- Signaling handlers ------------------------------
// These mirror PhoneCamDesktopApp's handleRemoteOffer etc. but inline so
// this single-file preview target doesn't depend on main.cpp.
class PreviewApp {
public:
    bool initialize() {
        securityManager_ = std::make_unique<SecurityManager>();

        sessionManager_ = std::make_unique<SessionManager>();
        if (!sessionManager_->initialize()) {
            std::cerr << "[Preview] SessionManager init failed" << std::endl;
            return false;
        }

        audioDistributor_ = std::make_unique<AudioDistributor>();
        if (!audioDistributor_->initialize()) {
            std::cerr << "[Preview] AudioDistributor init failed" << std::endl;
            return false;
        }

        ipcServer_ = std::make_unique<IpcServer>(securityManager_.get(), sessionManager_.get());
        if (!ipcServer_->start()) {
            std::cerr << "[Preview] IpcServer start failed" << std::endl;
            return false;
        }

        signalingServer_ = std::make_unique<SignalingServer>();

        SignalingServerConfig cfg;
        cfg.port = 8080;
        cfg.bindAddress = resolveBindAddress();
        cfg.maxConnections = 100;
        cfg.maxMessageSize = 256 * 1024;
        cfg.enablePingPong = true;
        cfg.pingInterval = std::chrono::seconds(30);
        cfg.connectionTimeout = std::chrono::seconds(60);
        cfg.requireAuthToken = false;   // milestone: matches SignalingClient.kt

        signalingServer_->setOnOfferCallback(
            [this](const std::string& sid, const std::string& did, const std::string& sdp) {
                onRemoteOffer(sid, did, sdp);
            });
        signalingServer_->setOnIceCandidateCallback(
            [this](const std::string& sid, const std::string& c) {
                sessionManager_->addIceCandidate(sid, c);
            });
        signalingServer_->setOnSessionInitCallback(
            [this](const std::string& sid, const std::string& did) {
                securityManager_->registerPairing(did, sid);
                securityManager_->generateSessionToken(sid, did);
                ensureSession(sid, did);
            });
        signalingServer_->setOnSessionCloseCallback(
            [this](const std::string& sid) {
                sessionManager_->removeSession(sid);
                audioDistributor_->removeSession(sid);
                securityManager_->revokeSessionToken(sid);
                {
                    std::lock_guard<std::mutex> lk(g_preview.mu);
                    if (g_preview.sessionId == sid) {
                        g_preview.state = "closed";
                    }
                }
                if (g_hWnd) updateWindowTitle();
            });
        signalingServer_->setOnErrorCallback(
            [](const std::string& sid, const std::string& err) {
                std::cerr << "[Preview] signaling err sid=" << sid << ": " << err << std::endl;
            });

        if (!signalingServer_->start(cfg)) {
            std::cerr << "[Preview] signaling start failed" << std::endl;
            return false;
        }

        // Forward libdatachannel-produced ICE candidates to the phone.
        sessionManager_->setIceCandidateHook(
            [this](const std::string& sid, const std::string& cand) {
                signalingServer_->sendIceCandidate(sid, cand);
            });

        // Wire up the video consumer that actually displays frames.
        sessionManager_->registerVideoConsumer(
            [](const std::string& sid, const VideoFrame& f) {
                onVideoFrame(sid, f);
            });

        // Wire up the audio consumer (for logging/monitoring in this preview)
        sessionManager_->registerAudioConsumer(
            [](const std::string& sid, const AudioFrame& f) {
                // Just log audio frame info for now - no playback in preview
                static std::mutex logMutex;
                std::lock_guard<std::mutex> lk(logMutex);
                static uint64_t frameCount = 0;
                frameCount++;
                if (frameCount % 100 == 1) {
                    std::cout << "[Preview] Audio frame: sid=" << sid
                              << " samples=" << f.data.size()
                              << " sr=" << f.sampleRate
                              << " ch=" << f.channels
                              << " ts=" << f.timestampUs << "us" << std::endl;
                }
            });

        if (cfg.bindAddress == "0.0.0.0" || cfg.bindAddress == "::") {
            std::cout
                << "[Preview] MILESTONE-DEV: signaling bound to all interfaces,"
                   " unauthenticated. Mirrors SignalingClient.kt bootstrap. DO NOT SHIP."
                << std::endl;
        }

        return true;
    }

    void shutdown() {
        if (signalingServer_) { signalingServer_->stop(); signalingServer_.reset(); }
        if (ipcServer_) { ipcServer_->stop(); ipcServer_.reset(); }
        if (sessionManager_) { sessionManager_->shutdown(); sessionManager_.reset(); }
        if (audioDistributor_) { audioDistributor_->shutdown(); audioDistributor_.reset(); }
        securityManager_.reset();
    }

private:
    void ensureSession(const std::string& sid, const std::string& did) {
        if (!sessionManager_->hasSession(sid)) {
            CameraSession s;
            s.deviceId = did;
            s.sessionId = sid;
            s.videoEnabled = true;
            s.audioEnabled = true;    // enable audio
            s.state = "connecting";
            sessionManager_->addSession(s);
        }
        if (!audioDistributor_->hasSession(sid)) {
            AudioSessionConfig ac;
            ac.sessionId = sid;
            ac.deviceId = did;
            ac.enabled = true;        // enable audio distributor
            audioDistributor_->addSession(ac);
        }
    }

    void onRemoteOffer(const std::string& sid, const std::string& did, const std::string& sdp) {
        std::cout << "[Preview] offer sid=" << sid << " dev=" << did
                  << " sdpBytes=" << sdp.size() << std::endl;
        ensureSession(sid, did);

        if (!sessionManager_->setRemoteOffer(sid, sdp)) {
            std::cerr << "[Preview] setRemoteOffer failed for " << sid << std::endl;
            return;
        }
        auto ans = sessionManager_->getLocalAnswer(sid);
        if (ans && signalingServer_) {
            if (signalingServer_->sendAnswer(sid, *ans)) {
                std::cout << "[Preview] sent answer (" << ans->size() << " bytes)" << std::endl;
            } else {
                std::cerr << "[Preview] sendAnswer failed" << std::endl;
            }
        } else {
            std::cerr << "[Preview] getLocalAnswer empty" << std::endl;
        }

        // Track the latest session's state for the title bar
        {
            std::lock_guard<std::mutex> lk(g_preview.mu);
            g_preview.sessionId = sid;
            g_preview.state = "connecting";
        }
    }

    static std::string resolveBindAddress() {
        // Match main.cpp: default to 0.0.0.0 for this milestone (LAN dev),
        // allow override via env var.
        std::string a;
#ifdef _WIN32
        char* v = nullptr; size_t n = 0;
        if (_dupenv_s(&v, &n, "PHONECAM_SIGNALING_BIND") == 0 && v) {
            a.assign(v, n - 1);
            free(v);
        }
#else
        const char* v = std::getenv("PHONECAM_SIGNALING_BIND");
        if (v) a = v;
#endif
        return a.empty() ? std::string("0.0.0.0") : a;
    }

    std::unique_ptr<SecurityManager> securityManager_;
    std::unique_ptr<SessionManager> sessionManager_;
    std::unique_ptr<AudioDistributor> audioDistributor_;
    std::unique_ptr<IpcServer> ipcServer_;
    std::unique_ptr<SignalingServer> signalingServer_;
};

// ------------------------- Win32 plumbing ----------------------------------

static void updateWindowTitle() {
    if (!g_hWnd) return;
    std::lock_guard<std::mutex> lk(g_preview.mu);
    char buf[512];
    if (g_preview.sessionId.empty()) {
        std::snprintf(buf, sizeof(buf), "%s - waiting for session...", kTitleBase);
    } else {
#ifdef PHONECAM_HAVE_VPX
        const char* sizePart = nullptr;
        char sizeBuf[64]{};
        if (g_preview.width > 0) {
            std::snprintf(sizeBuf, sizeof(sizeBuf), " %ux%u", g_preview.width, g_preview.height);
            sizePart = sizeBuf;
        } else {
            sizePart = "";
        }
#else
        const char* sizePart = " (depay-only, no libvpx)";
#endif
        std::snprintf(buf, sizeof(buf),
                      "%s - sid=%s state=%s%s fps=%.1f frames=%llu bytes=%llu",
                      kTitleBase,
                      g_preview.sessionId.c_str(),
                      g_preview.state.c_str(),
                      sizePart,
                      g_preview.measuredFps,
                      static_cast<unsigned long long>(g_preview.frameCount),
                      static_cast<unsigned long long>(g_preview.totalBytes));
    }
    SetWindowTextA(g_hWnd, buf);
}

static void renderFrame(HDC hdc, const RECT& rc) {
    std::lock_guard<std::mutex> lk(g_preview.mu);

    // Fill background
    HBRUSH bg = CreateSolidBrush(RGB(12, 16, 24));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(200, 220, 235));

#ifdef PHONECAM_HAVE_VPX
    if (g_preview.width > 0 && g_preview.height > 0 && !g_preview.rgb.empty()) {
        // Build BITMAPINFO each call; BMP caches the HBITMAP per size.
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(g_preview.width);
        // Negative height = top-down (we built rgb top-down).
        bmi.bmiHeader.biHeight = -static_cast<LONG>(g_preview.height);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 24;
        bmi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(hdc,
                      rc.left, rc.top,
                      rc.right - rc.left, rc.bottom - rc.top,
                      0, 0,
                      static_cast<int>(g_preview.width),
                      static_cast<int>(g_preview.height),
                      g_preview.rgb.data(),
                      &bmi,
                      DIB_RGB_COLORS,
                      SRCCOPY);
        return;
    }
#endif

    // State / diagnostic text. All values from g_preview, populated by real frames.
    char line[512];
    int y = 16;
    const int x = 16;
    const int lh = 22;

    std::snprintf(line, sizeof(line), "PhoneCam Desktop Preview (live)");
    TextOutA(hdc, x, y, line, (int)std::strlen(line)); y += lh;

    std::snprintf(line, sizeof(line), "Session:  %s",
                  g_preview.sessionId.empty() ? "<none>" : g_preview.sessionId.c_str());
    TextOutA(hdc, x, y, line, (int)std::strlen(line)); y += lh;

    std::snprintf(line, sizeof(line), "State:    %s", g_preview.state.c_str());
    TextOutA(hdc, x, y, line, (int)std::strlen(line)); y += lh;

    std::snprintf(line, sizeof(line), "Codec:    %s",
                  g_preview.codec.empty() ? "<none>" : g_preview.codec.c_str());
    TextOutA(hdc, x, y, line, (int)std::strlen(line)); y += lh;

    std::snprintf(line, sizeof(line), "Size:     %ux%u", g_preview.width, g_preview.height);
    TextOutA(hdc, x, y, line, (int)std::strlen(line)); y += lh;

    std::snprintf(line, sizeof(line), "Frames:   %llu",
                  static_cast<unsigned long long>(g_preview.frameCount));
    TextOutA(hdc, x, y, line, (int)std::strlen(line)); y += lh;

    std::snprintf(line, sizeof(line), "Total bytes: %llu",
                  static_cast<unsigned long long>(g_preview.totalBytes));
    TextOutA(hdc, x, y, line, (int)std::strlen(line)); y += lh;

    std::snprintf(line, sizeof(line), "Last RTP ts (us): %lld",
                  static_cast<long long>(g_preview.lastTimestampUs));
    TextOutA(hdc, x, y, line, (int)std::strlen(line)); y += lh;

    std::snprintf(line, sizeof(line), "Measured FPS: %.2f", g_preview.measuredFps);
    TextOutA(hdc, x, y, line, (int)std::strlen(line)); y += lh;

#ifdef PHONECAM_HAVE_VPX
    const char* mode = "Mode: libvpx decode enabled";
#else
    const char* mode =
        "Mode: depay-only (libvpx unavailable -- install via vcpkg and re-run CMake "
        "with -DPHONECAM_ENABLE_VPX_DECODE=ON and -DVPX_ROOT=<path>)";
#endif
    y += lh;
    std::snprintf(line, sizeof(line), "%s", mode);
    TextOutA(hdc, x, y, line, (int)std::strlen(line)); y += lh;
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            renderFrame(hdc, ps.rcPaint);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            g_running.store(false);
            PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND:
            return 1;   // prevent flicker
    }
    return DefWindowProcA(h, msg, w, l);
}

} // namespace phonecam

// ------------------------- Entry point -------------------------------------

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    (void)hInstance; (void)nCmdShow;
    using namespace phonecam;

    // Allocate a console for stdout/stderr diagnostics alongside the window.
    AllocConsole();
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    std::cout << "=== PhoneCamPreview ===" << std::endl;

    // Bring up the WebRTC + signaling stack.
    PreviewApp app;
    if (!app.initialize()) {
        std::cerr << "[Preview] initialize failed" << std::endl;
        return 1;
    }

    // Register Win32 window class and create the preview window.
    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassA(&wc)) {
        std::cerr << "[Preview] RegisterClass failed" << std::endl;
        return 1;
    }

    g_hWnd = CreateWindowExA(0, kClassName, kTitleBase,
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 960, 540,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hWnd) {
        std::cerr << "[Preview] CreateWindowEx failed" << std::endl;
        return 1;
    }
    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);
    updateWindowTitle();

    // Message pump
    MSG msg;
    while (g_running.load() && GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup
    app.shutdown();
    FreeConsole();
    return 0;
}
