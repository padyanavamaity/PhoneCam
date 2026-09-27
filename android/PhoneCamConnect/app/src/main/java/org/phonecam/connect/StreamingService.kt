package org.phonecam.connect

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Binder
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import org.phonecam.connect.stream.CameraConfigRepository
import org.phonecam.connect.stream.CameraFacing
import org.phonecam.connect.stream.SharedPrefsConfigStore
import org.phonecam.connect.stream.ServiceState
import org.phonecam.connect.stream.StreamEvent
import org.phonecam.connect.stream.StreamPhase
import org.phonecam.connect.stream.StreamStateMachine
import org.phonecam.connect.stream.WebRtcEngine
import org.webrtc.SurfaceViewRenderer

/**
 * Foreground service that owns the WebRTC-native camera pipeline.
 *
 * The CameraX/SurfaceTexture bridge is gone — this service delegates the media
 * pipeline entirely to [WebRtcEngine], which uses Camera2Enumerator +
 * CameraVideoCapturer per WebRTC M125, and tracks state with [StreamStateMachine].
 *
 * Scope (plan §3): the reachable end-state is CAMERA_ACTIVE. Signaling is
 * deferred, so WEBRTC_READY / CONNECTING / CONNECTED / STREAMING state machine
 * transitions are wired and unit-tested but unreachable at runtime.
 *
 * Threading contract: CameraVideoCapturer calls run on the main thread; the
 * engine internally marshals to the main Handler where needed.
 */
class StreamingService : Service() {

    private val TAG = "StreamingService"

    // ------------------------------------------------------------------
    // Public state for UI binding
    // ------------------------------------------------------------------

    private val _serviceState = MutableStateFlow(ServiceState())
    val serviceState: StateFlow<ServiceState> = _serviceState

    private val binder = LocalBinder()

    inner class LocalBinder : Binder() {
        fun getService(): StreamingService = this@StreamingService
    }

    override fun onBind(intent: Intent?): IBinder = binder

    // ------------------------------------------------------------------
    // Components
    // ------------------------------------------------------------------

    private var _engine: WebRtcEngine? = null
    private val engine: WebRtcEngine
        get() = _engine ?: throw IllegalStateException("WebRtcEngine accessed before onCreate")
    
    private var _configRepository: CameraConfigRepository? = null
    private val configRepository: CameraConfigRepository
        get() = _configRepository ?: throw IllegalStateException("CameraConfigRepository accessed before onCreate")
    
    // Fallback flow initialized at construction; replaced in onCreate
    private val fallbackPreviewReady = MutableStateFlow(false)

    private val machine = StreamStateMachine(
        onTransition = { from, to, event ->
            Log.d(TAG, "State: $from --${event::class.simpleName}--> $to")
            publishState()
        },
        onRejected = { phase, event ->
            Log.w(TAG, "Rejected event ${event::class.simpleName} in phase $phase")
        }
    )

    private val mainHandler = Handler(Looper.getMainLooper())
    private var serviceJob: Job? = null
    private var scope: CoroutineScope? = null

    // ------------------------------------------------------------------
    // Engine events (plan §5.2, §5.6)
    // ------------------------------------------------------------------

    private val engineEvents = object : WebRtcEngine.EngineEvents {
        override fun onCameraOpened() {
            machine.submit(StreamEvent.CameraOpened)
        }

        override fun onCameraFailed(message: String) {
            Log.e(TAG, "CameraFailed: $message")
            _serviceState.value = _serviceState.value.copy(errorMessage = message)
            machine.submit(StreamEvent.CameraFailed(message))
        }

        override fun onCameraSwitched(isFront: Boolean) {
            val facing = if (isFront) CameraFacing.FRONT else CameraFacing.BACK
            _serviceState.value = _serviceState.value.copy(cameraFacing = facing)
            val cfg = configRepository.load().copy(facing = facing)
            configRepository.save(cfg)
        }

        override fun onMeasuredStats(width: Int, height: Int, fps: Int) {
            _serviceState.value = _serviceState.value.copy(
                measuredWidth = width,
                measuredHeight = height,
                measuredFps = fps
            )
        }
    }

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    override fun onCreate() {
        super.onCreate()
        Log.d(TAG, "onCreate")

        // Notification channel
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "PhoneCam streaming", NotificationManager.IMPORTANCE_LOW)
        )

        _configRepository = CameraConfigRepository(SharedPrefsConfigStore(this)) {
            Log.d(TAG, "Config: $it")
        }
        _engine = WebRtcEngine(this, engineEvents)
        serviceJob = SupervisorJob()
        scope = CoroutineScope(Dispatchers.Default + serviceJob!!)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (!checkPermissions()) {
            Log.e(TAG, "Missing CAMERA/RECORD_AUDIO permissions — not starting")
            stopSelf()
            return START_NOT_STICKY
        }

        // Must call startForeground BEFORE camera open on Android 14 (FGS rules).
        startForeground(NOTIFICATION_ID, buildNotification())

        return when (intent?.action) {
            ACTION_STOP -> {
                requestStopInternal()
                START_NOT_STICKY
            }
            else -> {
                startRequested()
                START_STICKY
            }
        }
    }

    // ------------------------------------------------------------------
    // Start / stop
    // ------------------------------------------------------------------

    private fun startRequested() {
        if (!machine.submit(StreamEvent.StartRequested)) {
            Log.w(TAG, "StartRequested rejected in phase ${machine.phase}")
            return
        }

        engine.initialize()   // idempotent
        val cfg = configRepository.load()
        engine.startCamera(cfg)

        _serviceState.value = _serviceState.value.copy(cameraFacing = cfg.facing)
    }

    fun startStreaming() = startRequested()

    fun stopStreaming() = requestStopInternal()

    private fun requestStopInternal() {
        when (machine.phase) {
            StreamPhase.IDLE, StreamPhase.STOPPING -> return
            else -> machine.submit(StreamEvent.StopRequested)
        }

        // Stop capture synchronously on main thread; then teardown.
        engine.stop()
        machine.submit(StreamEvent.TeardownComplete)
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
        Log.d(TAG, "Stopped")
    }

    // ------------------------------------------------------------------
    // Public API for MainActivity / SettingsActivity
    // ------------------------------------------------------------------

    /**
     * Attach the shared-EGL [SurfaceViewRenderer] as a direct sink (no hop).
     * Must be called from the main thread.
     *
     * If the engine is not yet initialized, this will defer the attachment
     * until the engine's EGL context is ready.
     *
     * @return true if attached immediately, false if deferred.
     */
    fun attachPreview(renderer: SurfaceViewRenderer): Boolean {
        return engine.attachPreview(renderer)
    }

    fun detachPreview() {
        engine.detachPreview()
    }

    /** Whether the WebRTC engine's EGL context is ready for preview attachment. */
    val previewReady: StateFlow<Boolean>
        get() = _engine?.previewReady ?: fallbackPreviewReady

    /** Live-apply a new config if the camera is active; always persists. */
    fun applyConfig(config: org.phonecam.connect.stream.CameraStreamConfig) {
        configRepository.save(config)
        if (machine.phase == StreamPhase.CAMERA_ACTIVE) {
            engine.applyCaptureFormat(config.width, config.height, config.fps)
            // Facing change uses switchCamera when active
            if (config.facing != engine.activeConfig.facing) {
                engine.switchCamera()
            }
        }
    }

    // ------------------------------------------------------------------
    // Permissions + notification
    // ------------------------------------------------------------------

    private fun checkPermissions(): Boolean {
        val cam = ContextCompat.checkSelfPermission(this, android.Manifest.permission.CAMERA)
        val mic = ContextCompat.checkSelfPermission(this, android.Manifest.permission.RECORD_AUDIO)
        return cam == PackageManager.PERMISSION_GRANTED && mic == PackageManager.PERMISSION_GRANTED
    }

    private fun buildNotification(): Notification =
        Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("PhoneCam Camera")
            .setContentText("Camera active (local preview only)")
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setOngoing(true)
            .build()

    // ------------------------------------------------------------------
    // State publishing
    // ------------------------------------------------------------------

    private fun publishState() {
        val phase = machine.phase
        _serviceState.value = _serviceState.value.copy(
            phase = phase,
            isStreaming = phase == StreamPhase.STREAMING,
            micActive = phase == StreamPhase.CONNECTED || phase == StreamPhase.STREAMING,
            cameraActive = phase == StreamPhase.CAMERA_ACTIVE ||
                    phase == StreamPhase.WEBRTC_READY ||
                    phase == StreamPhase.CONNECTING ||
                    phase == StreamPhase.CONNECTED ||
                    phase == StreamPhase.STREAMING
        )
    }

    // ------------------------------------------------------------------
    // Teardown
    // ------------------------------------------------------------------

    override fun onDestroy() {
        super.onDestroy()
        Log.d(TAG, "onDestroy")

        // Activity unbinds first in onStop; preview is detached there.
        // Now release the engine (renderer before eglBase contract is honored).
        engine.release()
        publishState()

        serviceJob?.cancel()
        serviceJob = null
        scope = null
    }

    companion object {
        private const val CHANNEL_ID = "phonecam_stream"
        private const val NOTIFICATION_ID = 10
        const val ACTION_STOP = "org.phonecam.connect.action.STOP"
    }
}
