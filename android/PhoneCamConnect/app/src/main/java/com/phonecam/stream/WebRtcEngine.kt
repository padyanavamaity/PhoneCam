package com.phonecam.stream

import android.content.Context
import android.util.Log
import org.webrtc.AudioSource
import org.webrtc.AudioTrack
import org.webrtc.Camera2Enumerator
import org.webrtc.CameraEnumerationAndroid
import org.webrtc.CameraVideoCapturer
import org.webrtc.DataChannel
import org.webrtc.DefaultVideoDecoderFactory
import org.webrtc.DefaultVideoEncoderFactory
import org.webrtc.EglBase
import org.webrtc.IceCandidate
import org.webrtc.MediaConstraints
import org.webrtc.MediaStream
import org.webrtc.PeerConnection
import org.webrtc.PeerConnectionFactory
import org.webrtc.RtpSender
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import org.webrtc.SurfaceTextureHelper
import org.webrtc.VideoSink
import org.webrtc.VideoSource
import org.webrtc.VideoTrack
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import kotlin.math.abs

/**
 * Owns camera capture, microphone, tracks and the PeerConnection.
 * Created and disposed ONLY by StreamingService.
 *
 * Camera/mic lifetime is independent of the PeerConnection lifetime:
 *   camera+mic  : startCamera() ... dispose()
 *   session     : startSession() ... closeSession()   (many per engine lifetime)
 */
class WebRtcEngine(private val context: Context, private val listener: Listener) {

    interface Listener {
        fun onLocalDescription(sdp: SessionDescription)
        fun onLocalIceCandidate(candidate: IceCandidate)
        fun onPeerState(state: PeerConnection.PeerConnectionState)
        /** true when outbound video bytes are actually increasing. */
        fun onMediaFlowChanged(flowing: Boolean)
        fun onEngineError(message: String)
    }

    val eglBase: EglBase = EglBase.create()

    private val factory: PeerConnectionFactory
    private val videoSource: VideoSource
    private val videoTrack: VideoTrack
    private val audioSource: AudioSource
    private val audioTrack: AudioTrack
    private val helper: SurfaceTextureHelper

    private var capturer: CameraVideoCapturer? = null
    private var peer: PeerConnection? = null
    private var videoSender: RtpSender? = null
    private var previewSink: VideoSink? = null
    private var generation = 0
    private var disposed = false
    private var bitrateKbps = 4000

    @Volatile var currentMode: CameraMode? = null
        private set
    @Volatile var currentFacing: CameraFacing = CameraFacing.BACK
        private set
    val isCameraRunning: Boolean get() = capturer != null

    private val scheduler = Executors.newSingleThreadScheduledExecutor()
    private var statsFuture: ScheduledFuture<*>? = null
    private var lastBytes = -1L
    private var lastFlowing = false

    init {
        ensureInitialized(context)
        factory = PeerConnectionFactory.builder()
            .setVideoEncoderFactory(DefaultVideoEncoderFactory(eglBase.eglBaseContext, true, true))
            .setVideoDecoderFactory(DefaultVideoDecoderFactory(eglBase.eglBaseContext))
            .createPeerConnectionFactory()

        helper = SurfaceTextureHelper.create("PhoneCamCapture", eglBase.eglBaseContext)
        videoSource = factory.createVideoSource(false)
        videoTrack = factory.createVideoTrack("phonecam-video", videoSource).also { it.setEnabled(true) }
        audioSource = factory.createAudioSource(MediaConstraints())
        audioTrack = factory.createAudioTrack("phonecam-audio", audioSource).also { it.setEnabled(true) }
    }

    // ---------------------------------------------------------------- camera

    /**
     * Starts (or restarts) capture on the requested camera using the supported mode
     * closest to the request. The device/mode is resolved BEFORE stopping the old
     * capturer, so an unsupported request does not kill a working camera.
     */
    @Synchronized
    fun startCamera(facing: CameraFacing, width: Int, height: Int, fps: Int): CameraMode {
        check(!disposed) { "Engine disposed" }
        val enumerator = Camera2Enumerator(context)
        val device = enumerator.deviceNames.firstOrNull {
            if (facing == CameraFacing.FRONT) enumerator.isFrontFacing(it) else enumerator.isBackFacing(it)
        } ?: throw IllegalStateException("This device has no ${facing.name.lowercase()} camera")

        val formats = enumerator.getSupportedFormats(device).orEmpty()
        val mode = chooseMode(formats, width, height, fps)
            ?: throw IllegalStateException("Camera reports no supported capture formats")

        val newCapturer = enumerator.createCapturer(device, cameraEvents)
            ?: throw IllegalStateException("Could not open camera $device")

        stopCameraInternal()
        newCapturer.initialize(helper, context, videoSource.capturerObserver)
        newCapturer.startCapture(mode.width, mode.height, mode.fps)
        capturer = newCapturer
        currentFacing = facing
        currentMode = mode
        return mode
    }

    private fun stopCameraInternal() {
        capturer?.let {
            try { it.stopCapture() } catch (e: InterruptedException) { Thread.currentThread().interrupt() }
            it.dispose()
        }
        capturer = null
    }

    private val cameraEvents = object : CameraVideoCapturer.CameraEventsHandler {
        override fun onCameraError(errorDescription: String?) {
            listener.onEngineError("Camera error: $errorDescription")
        }
        override fun onCameraDisconnected() { listener.onEngineError("Camera disconnected") }
        override fun onCameraFreezed(errorDescription: String?) {
            listener.onEngineError("Camera froze: $errorDescription")
        }
        override fun onCameraOpening(cameraName: String?) {}
        override fun onFirstFrameAvailable() {}
        override fun onCameraClosed() {}
    }

    private fun chooseMode(
        formats: List<CameraEnumerationAndroid.CaptureFormat>, w: Int, h: Int, fps: Int
    ): CameraMode? {
        val best = formats.minByOrNull { f ->
            val dims = abs(f.width - w).toLong() + abs(f.height - h).toLong()
            val maxFps = f.framerate.max / 1000
            val fpsPenalty = if (maxFps >= fps) 0L else (fps - maxFps).toLong()
            dims * 100 + fpsPenalty
        } ?: return null
        val lo = best.framerate.min / 1000
        val hi = maxOf(lo, best.framerate.max / 1000)
        return CameraMode(best.width, best.height, fps.coerceIn(lo, hi))
    }

    // ----------------------------------------------------------------- audio

    fun setMicEnabled(enabled: Boolean) { audioTrack.setEnabled(enabled) }

    // --------------------------------------------------------------- preview

    @Synchronized
    fun attachPreview(sink: VideoSink) {
        if (disposed) return
        previewSink?.let { videoTrack.removeSink(it) }
        videoTrack.addSink(sink)
        previewSink = sink
    }

    @Synchronized
    fun detachPreview(sink: VideoSink) {
        if (previewSink === sink) {
            if (!disposed) videoTrack.removeSink(sink)
            previewSink = null
        }
    }

    // --------------------------------------------------------------- session

    fun setBitrateKbps(kbps: Int) {
        bitrateKbps = kbps
        applyBitrate()
    }

    private fun applyBitrate() {
        val sender = videoSender ?: return
        try {
            val params = sender.parameters
            if (params.encodings.isEmpty()) return
            for (enc in params.encodings) enc.maxBitrateBps = bitrateKbps * 1000
            sender.setParameters(params)
        } catch (e: Exception) {
            Log.w(TAG, "Could not apply bitrate", e)
        }
    }

    /** Creates a PeerConnection, adds tracks and produces an SDP offer. */
    @Synchronized
    fun startSession() {
        check(!disposed) { "Engine disposed" }
        closeSession()
        val gen = ++generation

        val config = PeerConnection.RTCConfiguration(emptyList()).apply {
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
            continualGatheringPolicy = PeerConnection.ContinualGatheringPolicy.GATHER_CONTINUALLY
        }
        val pc = factory.createPeerConnection(config, observer(gen))
        if (pc == null) {
            listener.onEngineError("Could not create PeerConnection")
            return
        }
        peer = pc
        videoSender = pc.addTrack(videoTrack, listOf("phonecam-stream"))
        pc.addTrack(audioTrack, listOf("phonecam-stream"))

        pc.createOffer(object : SimpleSdpObserver() {
            override fun onCreateSuccess(sdp: SessionDescription?) {
                if (sdp == null || gen != generation) return
                // Send the offer BEFORE setLocalDescription so that ICE candidates
                // (which only start after it) can never overtake the offer.
                listener.onLocalDescription(sdp)
                pc.setLocalDescription(object : SimpleSdpObserver() {
                    override fun onSetSuccess() { applyBitrate() }
                    override fun onSetFailure(error: String?) {
                        listener.onEngineError("setLocalDescription failed: $error")
                    }
                }, sdp)
            }
            override fun onCreateFailure(error: String?) {
                listener.onEngineError("createOffer failed: $error")
            }
        }, MediaConstraints())
    }

    fun setRemoteAnswer(sdp: String) {
        val pc = peer ?: return
        pc.setRemoteDescription(object : SimpleSdpObserver() {
            override fun onSetSuccess() { applyBitrate() }
            override fun onSetFailure(error: String?) {
                listener.onEngineError("setRemoteDescription failed: $error")
            }
        }, SessionDescription(SessionDescription.Type.ANSWER, sdp))
    }

    fun addRemoteCandidate(mid: String?, index: Int, sdp: String) {
        peer?.addIceCandidate(IceCandidate(mid, index, sdp))
    }

    @Synchronized
    fun closeSession() {
        generation++            // invalidate callbacks from the old connection
        stopStats()
        videoSender = null
        peer?.let {
            try { it.close() } catch (_: Exception) {}
            try { it.dispose() } catch (_: Exception) {}
        }
        peer = null
    }

    private fun observer(gen: Int) = object : PeerConnection.Observer {
        override fun onSignalingChange(s: PeerConnection.SignalingState?) {}
        override fun onIceConnectionChange(s: PeerConnection.IceConnectionState?) {}
        override fun onIceConnectionReceivingChange(b: Boolean) {}
        override fun onIceGatheringChange(s: PeerConnection.IceGatheringState?) {}
        override fun onIceCandidate(c: IceCandidate?) {
            if (gen == generation && c != null) listener.onLocalIceCandidate(c)
        }
        override fun onIceCandidatesRemoved(c: Array<out IceCandidate>?) {}
        override fun onAddStream(s: MediaStream?) {}
        override fun onRemoveStream(s: MediaStream?) {}
        override fun onDataChannel(d: DataChannel?) {}
        override fun onRenegotiationNeeded() {}
        override fun onConnectionChange(s: PeerConnection.PeerConnectionState?) {
            if (gen != generation || s == null) return
            when (s) {
                PeerConnection.PeerConnectionState.CONNECTED -> startStats()
                PeerConnection.PeerConnectionState.FAILED,
                PeerConnection.PeerConnectionState.CLOSED -> stopStats()
                else -> {}
            }
            listener.onPeerState(s)
        }
    }

    // ----------------------------------------------------------------- stats

    private fun startStats() {
        stopStats()
        lastBytes = -1L
        lastFlowing = false
        statsFuture = scheduler.scheduleWithFixedDelay({ pollStats() }, 1, 2, TimeUnit.SECONDS)
    }

    private fun stopStats() {
        statsFuture?.cancel(false)
        statsFuture = null
        if (lastFlowing) { lastFlowing = false; listener.onMediaFlowChanged(false) }
    }

    private fun pollStats() {
        val pc = peer ?: return
        try {
            pc.getStats { report ->
                var bytes = -1L
                for (s in report.statsMap.values) {
                    if (s.type == "outbound-rtp" && s.members["kind"] == "video") {
                        (s.members["bytesSent"] as? Number)?.let { bytes = it.toLong() }
                    }
                }
                val flowing = lastBytes >= 0 && bytes > lastBytes
                if (bytes >= 0) lastBytes = bytes
                if (flowing != lastFlowing) {
                    lastFlowing = flowing
                    listener.onMediaFlowChanged(flowing)
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "stats failed", e)
        }
    }

    // --------------------------------------------------------------- dispose

    @Synchronized
    fun dispose() {
        if (disposed) return
        closeSession()
        stopCameraInternal()
        previewSink?.let { videoTrack.removeSink(it) }
        previewSink = null
        disposed = true
        scheduler.shutdownNow()
        videoTrack.dispose()
        videoSource.dispose()
        audioTrack.dispose()
        audioSource.dispose()
        helper.dispose()
        factory.dispose()
        eglBase.release()
    }

    private open class SimpleSdpObserver : SdpObserver {
        override fun onCreateSuccess(sdp: SessionDescription?) {}
        override fun onSetSuccess() {}
        override fun onCreateFailure(error: String?) {}
        override fun onSetFailure(error: String?) {}
    }

    companion object {
        private const val TAG = "WebRtcEngine"
        private var initialized = false

        @Synchronized
        private fun ensureInitialized(ctx: Context) {
            if (initialized) return
            PeerConnectionFactory.initialize(
                PeerConnectionFactory.InitializationOptions.builder(ctx.applicationContext)
                    .createInitializationOptions()
            )
            initialized = true
        }

        /** Resolutions the camera actually supports, largest first. */
        fun supportedResolutions(ctx: Context, facing: CameraFacing): List<Pair<Int, Int>> = try {
            val e = Camera2Enumerator(ctx)
            val dev = e.deviceNames.firstOrNull {
                if (facing == CameraFacing.FRONT) e.isFrontFacing(it) else e.isBackFacing(it)
            }
            if (dev == null) emptyList() else e.getSupportedFormats(dev).orEmpty()
                .map { it.width to it.height }
                .filter { it.first >= 320 }
                .distinct()
                .sortedByDescending { it.first * it.second }
        } catch (_: Exception) { emptyList() }

        /** Highest FPS the camera reports for the given resolution. */
        fun maxFps(ctx: Context, facing: CameraFacing, w: Int, h: Int): Int = try {
            val e = Camera2Enumerator(ctx)
            val dev = e.deviceNames.first {
                if (facing == CameraFacing.FRONT) e.isFrontFacing(it) else e.isBackFacing(it)
            }
            e.getSupportedFormats(dev).orEmpty()
                .filter { it.width == w && it.height == h }
                .maxOfOrNull { it.framerate.max / 1000 } ?: 30
        } catch (_: Exception) { 30 }
    }
}