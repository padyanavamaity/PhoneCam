package org.phonecam.connect.stream

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import org.webrtc.Camera2Enumerator
import org.webrtc.CameraVideoCapturer
import org.webrtc.DefaultVideoDecoderFactory
import org.webrtc.DefaultVideoEncoderFactory
import org.webrtc.EglBase
import org.webrtc.MediaConstraints
import org.webrtc.PeerConnection
import org.webrtc.PeerConnectionFactory
import org.webrtc.RtpParameters
import org.webrtc.MediaStreamTrack
import org.webrtc.RendererCommon
import org.webrtc.SurfaceTextureHelper
import org.webrtc.SurfaceViewRenderer
import org.webrtc.VideoFrame
import org.webrtc.VideoSink
import org.webrtc.VideoSource
import org.webrtc.VideoTrack
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit

/**
 * WebRTC-native camera pipeline (plan §2, §5).
 *
 * Owns: EglBase, PeerConnectionFactory, JavaAudioDeviceModule, Camera2 capturer,
 * SurfaceTextureHelper, VideoSource/VideoTrack, and the local preview attach.
 *
 * Threading contract (plan §2):
 *  - All CameraVideoCapturer calls on the main thread.
 *  - PeerConnectionFactory.initialize once per process.
 *  - SurfaceViewRenderer.init / addSink / removeSink from the main thread.
 *  - Fixed disposal order: stopCapture → capturer.dispose → helper.dispose →
 *    tracks → sources → factory → ADM.release → eglBase.release.
 */
class WebRtcEngine(
    private val context: Context,
    private val engineEvents: EngineEvents
) {
    private val TAG = "WebRtcEngine"

    interface EngineEvents {
        fun onCameraOpened()
        fun onCameraFailed(message: String)
        fun onCameraSwitched(isFront: Boolean)
        fun onMeasuredStats(width: Int, height: Int, fps: Int)
    }

    class CameraUnavailableException(message: String) : Exception(message)

    // Core WebRTC objects
    private var eglBase: EglBase? = null
    private var factory: PeerConnectionFactory? = null
    private var audioEngine: AudioEngine? = null

    private var videoCapturer: CameraVideoCapturer? = null
    private var surfaceTextureHelper: SurfaceTextureHelper? = null
    private var videoSource: VideoSource? = null
    private var videoTrack: VideoTrack? = null

    private var previewRenderer: SurfaceViewRenderer? = null
    private var pendingPreviewRenderer: SurfaceViewRenderer? = null

    // Measured-stats sink (plan §5.6) — counts frames to produce real FPS
    private var statsSink: VideoSink? = null
    private var statsExecutor: ScheduledExecutorService? = null
    @Volatile private var frameCountInWindow: Int = 0
    @Volatile private var lastFrameWidth: Int = 0
    @Volatile private var lastFrameHeight: Int = 0

    var activeConfig: CameraStreamConfig = CameraStreamConfig.DEFAULT
        private set

    private val mainHandler = Handler(Looper.getMainLooper())

    // Preview lifecycle state
    private val _previewReady = MutableStateFlow(false)
    val previewReady: StateFlow<Boolean> = _previewReady

    // ------------------------------------------------------------------
    // Public lifecycle
    // ------------------------------------------------------------------

    /**
     * Initialize EGL, ADM, and the PeerConnectionFactory. Idempotent.
     * Must be called before [startCamera].
     *
     * After this completes, [previewReady] will emit true and any pending
     * preview attachment will be performed.
     */
    @Synchronized
    fun initialize() {
        if (factory != null) {
            Log.d(TAG, "initialize() ignored — already initialized")
            return
        }

        Log.d(TAG, "Initializing WebRTC: EglBase + PeerConnectionFactory")

        // EglBase first — everything else shares this context.
        val egl = EglBase.create()
        eglBase = egl

        // Process-level factory initialization (once per process — guard via field).
        val initOptions = PeerConnectionFactory.InitializationOptions.builder(context)
            .setEnableInternalTracer(false)
            .createInitializationOptions()
        PeerConnectionFactory.initialize(initOptions)

        // Explicit JavaAudioDeviceModule (plan §5.5) before building the factory.
        val audio = AudioEngine(context)
        val factoryBuilder = PeerConnectionFactory.builder()
            .setOptions(PeerConnectionFactory.Options())
            .setVideoEncoderFactory(DefaultVideoEncoderFactory(egl.eglBaseContext, true, true))
            .setVideoDecoderFactory(DefaultVideoDecoderFactory(egl.eglBaseContext))
        audio.initialize(factoryBuilder)

        factory = factoryBuilder.createPeerConnectionFactory()
        audioEngine = audio

        // Create audio source/track now — NOT capturing. The ADM only opens the
        // mic when an active PeerConnection pulls this AudioSource (plan §3.4).
        audio.createTrack(factory!!)

        // EGL is now ready — notify listeners and attach any pending preview
        _previewReady.value = true
        mainHandler.post { attachPendingPreviewIfNeeded() }
        Log.d(TAG, "Engine initialized, EGL ready")
    }

    /**
     * Open the camera and start capture. Caller must have CAMERA permission
     * and be in a foreground-service context (Android 14 FGS rules).
     * All capturer work is posted to the main thread per M125 contract.
     */
    fun startCamera(config: CameraStreamConfig) {
        check(factory != null) { "initialize() must run before startCamera()" }

        mainHandler.post {
            try {
                startCameraOnMain(config)
            } catch (e: CameraUnavailableException) {
                engineEvents.onCameraFailed(e.message ?: "camera unavailable")
            } catch (e: Exception) {
                Log.e(TAG, "startCamera failed", e)
                engineEvents.onCameraFailed(e.message ?: "startCapture failed")
            }
        }
    }

    private fun startCameraOnMain(config: CameraStreamConfig) {
        val enumerator = Camera2Enumerator(context)
        val deviceName = selectCameraDevice(enumerator, config.facing)
            ?: throw CameraUnavailableException("No ${config.facing} camera found")

        val capturer = enumerator.createCapturer(deviceName, cameraEventsHandler)
            ?: throw CameraUnavailableException("createCapturer returned null for $deviceName")

        // Video source + helper — capturer feeds frames into the source via the helper.
        val source = factory!!.createVideoSource(false)  // isScreencast = false
        val helper = SurfaceTextureHelper.create("CameraTexture", eglBase!!.eglBaseContext)
        capturer.initialize(helper, context, source.capturerObserver)

        // Adapt/sink to requested output format.
        source.adaptOutputFormat(config.width, config.height, config.fps)

        val track = factory!!.createVideoTrack(VIDEO_TRACK_ID, source)

        videoSource = source
        surfaceTextureHelper = helper
        videoTrack = track
        videoCapturer = capturer
        activeConfig = config

        // Attach measured-stats sink to the track (frame counting, plan §5.6).
        attachStatsSink(track)

        // If a preview renderer was attached before the video track existed
        // (e.g. by attachPendingPreviewIfNeeded during initialize()), hook it up
        // now so frames actually flow to the surface.
        previewRenderer?.let { track.addSink(it) }

        // startCapture picks nearest supported Camera2 size internally.
        capturer.startCapture(config.width, config.height, config.fps)
        Log.d(TAG, "startCapture(${config.width}x${config.height}@${config.fps}) on $deviceName")
    }

    // ------------------------------------------------------------------
    // Camera enumeration (no camera open, no permission needed) — plan §5.1
    // ------------------------------------------------------------------

    fun supportedFormats(facing: CameraFacing): List<SizeFps> {
        val enumerator = Camera2Enumerator(context)
        val name = selectCameraDevice(enumerator, facing) ?: return emptyList()
        val rawFormats = enumerator.getSupportedFormats(name)
            ?: return emptyList()
        
        // Debug: log raw formats to diagnose missing FPS options
        rawFormats.forEach { sz ->
            Log.d(TAG, "Raw format: ${sz.width}x${sz.height} framerate=[${sz.framerate.min/1000}..${sz.framerate.max/1000}] fps")
        }
        
        return rawFormats
            .map { SizeFps(it.width, it.height, it.framerate.max / 1000) } // milli-fps
            .distinct()
            .sortedWith(compareByDescending<SizeFps> { it.width * it.height }.thenByDescending { it.fps })
    }

    private fun selectCameraDevice(enumerator: Camera2Enumerator, facing: CameraFacing): String? =
        enumerator.deviceNames.firstOrNull { name ->
            when (facing) {
                CameraFacing.BACK -> enumerator.isBackFacing(name)
                CameraFacing.FRONT -> enumerator.isFrontFacing(name)
            }
        }

    // ------------------------------------------------------------------
    // Live camera controls (plan §5.7, §5.8)
    // ------------------------------------------------------------------

    fun switchCamera() {
        mainHandler.post {
            videoCapturer?.switchCamera(object : CameraVideoCapturer.CameraSwitchHandler {
                override fun onCameraSwitchDone(isFrontCamera: Boolean) {
                    activeConfig = activeConfig.copy(
                        facing = if (isFrontCamera) CameraFacing.FRONT else CameraFacing.BACK
                    )
                    previewRenderer?.setMirror(isFrontCamera)
                    engineEvents.onCameraSwitched(isFrontCamera)
                }

                override fun onCameraSwitchError(errorDescription: String) {
                    engineEvents.onCameraFailed("Switch failed: $errorDescription")
                }
            })
        }
    }

    /** Live format change — no teardown. Bitrate/codec are NOT applied here. */
    fun applyCaptureFormat(width: Int, height: Int, fps: Int) {
        mainHandler.post {
            activeConfig = activeConfig.copy(width = width, height = height, fps = fps)
            videoSource?.adaptOutputFormat(width, height, fps)
            videoCapturer?.changeCaptureFormat(width, height, fps)
        }
    }

    // ------------------------------------------------------------------
    // Preview (plan §5.6)
    // ------------------------------------------------------------------

    /**
     * Attach the shared-EGL [SurfaceViewRenderer] directly as a sink (no hop).
     * Must be called on the main thread.
     *
     * Fail-safe behavior:
     * - If EGL is not ready (initialize() not yet called), defers the attachment
     *   and logs a diagnostic. The pending renderer will be attached once EGL
     *   becomes ready via [initialize].
     * - If EGL is ready, attaches immediately.
     *
     * @return true if attached immediately, false if deferred or failed.
     */
    fun attachPreview(renderer: SurfaceViewRenderer): Boolean {
        check(Looper.myLooper() == Looper.getMainLooper()) {
            "attachPreview must be called on the main thread"
        }

        val egl = eglBase
        if (egl == null) {
            Log.w(TAG, "attachPreview: EGL not ready — deferring preview attachment")
            pendingPreviewRenderer = renderer
            return false
        }

        // If we already have a preview attached, detach it first
        detachPreviewInternal()

        return try {
            renderer.init(egl.eglBaseContext, null)
            renderer.setScalingType(RendererCommon.ScalingType.SCALE_ASPECT_FIT)
            renderer.setEnableHardwareScaler(true)
            renderer.setMirror(activeConfig.facing == CameraFacing.FRONT)
            previewRenderer = renderer
            videoTrack?.addSink(renderer)
            Log.d(TAG, "Preview attached (mirror=${activeConfig.facing == CameraFacing.FRONT})")
            true
        } catch (e: Exception) {
            Log.e(TAG, "attachPreview failed", e)
            pendingPreviewRenderer = renderer
            false
        }
    }

    fun detachPreview() {
        check(Looper.myLooper() == Looper.getMainLooper()) {
            "detachPreview must be called on the main thread"
        }
        detachPreviewInternal()
        Log.d(TAG, "Preview detached")
    }

    /**
     * Called when EGL becomes ready to attach any pending preview.
     * Internal use only.
     */
    private fun attachPendingPreviewIfNeeded() {
        val pending = pendingPreviewRenderer ?: return
        if (eglBase != null) {
            Log.d(TAG, "Attaching pending preview after EGL ready")
            pendingPreviewRenderer = null
            attachPreview(pending)
        }
    }

    // ------------------------------------------------------------------
    // Measured stats (plan §5.6) — real frame counting, no fakes
    // ------------------------------------------------------------------

    private fun attachStatsSink(track: VideoTrack) {
        val exec = Executors.newSingleThreadScheduledExecutor { r ->
            Thread(r, "pc-stats").apply { isDaemon = true }
        }
        statsExecutor = exec

        val sink = object : VideoSink {
            override fun onFrame(frame: VideoFrame) {
                frameCountInWindow++
                lastFrameWidth = frame.rotatedWidth
                lastFrameHeight = frame.rotatedHeight
            }
        }
        statsSink = sink
        track.addSink(sink)

        exec.scheduleAtFixedRate({
            val fps = frameCountInWindow
            frameCountInWindow = 0
            if (fps > 0) {
                engineEvents.onMeasuredStats(lastFrameWidth, lastFrameHeight, fps)
            }
        }, 1, 1, TimeUnit.SECONDS)
    }

    private fun detachStatsSink() {
        statsSink?.let { stats ->
            runCatching { videoTrack?.removeSink(stats) }
        }
        statsSink = null
        runCatching {
            statsExecutor?.shutdown()
            statsExecutor?.awaitTermination(1, TimeUnit.SECONDS)
        }
        statsExecutor = null
    }

    // ------------------------------------------------------------------
    // RTP preferences (plan §7) — consumed by the future signaling task
    // ------------------------------------------------------------------

    /** Apply video max-bitrate hint on an established PeerConnection (future). */
    fun applyRtpPreferences(peerConnection: PeerConnection, config: CameraStreamConfig) {
        peerConnection.senders.forEach { sender ->
            val track: MediaStreamTrack? = sender.track()
            if (track?.kind() == MediaStreamTrack.VIDEO_TRACK_KIND) {
                val params: RtpParameters = sender.parameters
                params.encodings.forEach { it.maxBitrateBps = config.videoBitrateBps }
                sender.parameters = params
            }
        }
        // Codec preference via RtpTransceiver.setCodecPreferences — VERIFY #2 in plan §10.
    }

    // ------------------------------------------------------------------
    // Teardown (plan §7.9) — fixed order
    // ------------------------------------------------------------------

    /**
     * Stop capture and release all resources. Safe to call from any thread;
     * capturer work is marshaled to the main thread.
     */
    fun stop() {
        // Detach stats first so no frames are counted during teardown.
        detachStatsSink()

        // Clear any pending preview
        pendingPreviewRenderer = null

        // Detach preview before releasing EGL (contract: renderer before eglBase).
        if (Looper.myLooper() == Looper.getMainLooper()) {
            detachPreviewInternal()
        } else {
            mainHandler.post { detachPreviewInternal() }
        }

        // Capturer stop/dispose on main thread.
        if (Looper.myLooper() == Looper.getMainLooper()) {
            stopCaptureOnMain()
        } else {
            mainHandler.post { stopCaptureOnMain() }
        }
    }

    private fun stopCaptureOnMain() {
        videoCapturer?.let {
            try {
                it.stopCapture()
            } catch (e: InterruptedException) {
                Thread.currentThread().interrupt()
            } catch (e: Exception) {
                Log.e(TAG, "stopCapture error", e)
            }
        }
        runCatching { videoCapturer?.dispose() }
        videoCapturer = null

        runCatching { surfaceTextureHelper?.dispose() }
        surfaceTextureHelper = null

        runCatching { videoTrack?.dispose() }
        videoTrack = null
        runCatching { videoSource?.dispose() }
        videoSource = null

        Log.d(TAG, "Camera pipeline stopped")
    }

    private fun detachPreviewInternal() {
        previewRenderer?.let { r ->
            runCatching { videoTrack?.removeSink(r) }
            runCatching { r.release() }
        }
        previewRenderer = null
    }

    /**
     * Full engine teardown (service onDestroy). Must run after any Activity has
     * detached its preview (unbind happens first in onStop).
     */
    @Synchronized
    fun release() {
        _previewReady.value = false
        stop()

        if (Looper.myLooper() == Looper.getMainLooper()) {
            releaseFactoryOnMain()
        } else {
            mainHandler.post { releaseFactoryOnMain() }
        }
    }

    private fun releaseFactoryOnMain() {
        runCatching { factory?.dispose() }
        factory = null

        audioEngine?.release()
        audioEngine = null

        runCatching { eglBase?.release() }
        eglBase = null

        Log.d(TAG, "Engine released")
    }

    // ------------------------------------------------------------------
    // Capturer events (plan §5.2)
    // ------------------------------------------------------------------

    private val cameraEventsHandler = object : CameraVideoCapturer.CameraEventsHandler {
        override fun onCameraError(errorDescription: String) {
            engineEvents.onCameraFailed(errorDescription)
        }
        override fun onCameraDisconnected() {
            engineEvents.onCameraFailed("Camera disconnected")
        }
        override fun onCameraFreezed(errorDescription: String) {
            // Recoverable — camera2 capturer auto-thaws; log only.
            Log.w(TAG, "onCameraFreezed: $errorDescription")
        }
        override fun onCameraOpening(cameraName: String) {
            Log.d(TAG, "onCameraOpening: $cameraName")
        }
        override fun onFirstFrameAvailable() {
            // First frame means the camera pipeline is up — notify state machine.
            // This callback may come from camera thread; post to main for safety.
            mainHandler.post {
                Log.d(TAG, "onFirstFrameAvailable -> onCameraOpened")
                engineEvents.onCameraOpened()
            }
        }
        override fun onCameraClosed() {
            Log.d(TAG, "onCameraClosed")
        }
    }

    companion object {
        const val VIDEO_TRACK_ID = "ARDAMSv0"
    }
}
