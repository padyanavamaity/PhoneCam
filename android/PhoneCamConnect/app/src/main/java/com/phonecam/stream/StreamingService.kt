package com.phonecam.stream

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.flow.update
import org.json.JSONObject
import org.webrtc.IceCandidate
import org.webrtc.PeerConnection
import org.webrtc.SessionDescription
import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.UUID
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * Foreground service that OWNS the streaming lifecycle (spec §3, §6):
 * camera, microphone, WebRtcEngine, signaling server, mDNS advertisement.
 * MainActivity is only a UI that observes StreamingRepository.
 *
 * All state mutation happens on the single-thread [executor] to avoid races.
 */
class StreamingService : Service(), WebRtcEngine.Listener, SignalingServer.Callbacks {

    private val repo = StreamingRepository
    private lateinit var settings: AppSettings
    private lateinit var executor: ExecutorService

    private var engine: WebRtcEngine? = null
    private var server: SignalingServer? = null
    private var serverPort = 0

    private var nsd: NsdManager? = null
    private var nsdListener: NsdManager.RegistrationListener? = null

    private var multicastLock: WifiManager.MulticastLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var wakeLock: PowerManager.WakeLock? = null

    @Volatile private var sessionId: String = ""
    private var peerConnected = false
    private var mediaFlowing = false
    private var started = false
    private var stopped = false
    private var lastRequestedMode: Triple<Int, Int, Int>? = null

    // ------------------------------------------------------------ lifecycle

    override fun onCreate() {
        super.onCreate()
        settings = AppSettings(this)
        executor = Executors.newSingleThreadExecutor()
        val ch = NotificationChannel(CHANNEL_ID, "Camera streaming", NotificationManager.IMPORTANCE_LOW)
        (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(ch)
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                enterForeground()              // must happen promptly after startForegroundService
                if (!started) {
                    started = true
                    executor.execute { startAll() }
                }
            }
            ACTION_STOP -> {
                if (started) executor.execute { stopAll(StreamState.IDLE, null) } else stopSelf()
            }
            ACTION_SET_MIC -> onExecutor {
                doSetMic(intent?.getBooleanExtra(EXTRA_ENABLED, true) ?: true); sendStatus()
            }
            ACTION_SET_FACING -> onExecutor {
                val f = runCatching { CameraFacing.valueOf(intent?.getStringExtra(EXTRA_FACING) ?: "") }.getOrNull()
                if (f != null) doSetFacing(f); sendStatus()
            }
            ACTION_APPLY_SETTINGS -> onExecutor { doApplySettings(); sendStatus() }
            else -> if (!started) stopSelf()
        }
        return START_NOT_STICKY
    }

    private fun onExecutor(block: () -> Unit) {
        if (!started) { stopSelf(); return }
        executor.execute { runCatching(block).onFailure { Log.w(TAG, "command failed", it) } }
    }

    override fun onDestroy() {
        if (started && !stopped) stopAll(StreamState.IDLE, null)
        executor.shutdown()
        super.onDestroy()
    }

    private fun enterForeground() {
        val type = if (Build.VERSION.SDK_INT >= 30)
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        else 0
        ServiceCompat.startForeground(this, NOTIF_ID, buildNotification(), type)
    }

    // ------------------------------------------------------------- startup

    private fun startAll() {
        try {
            setState(StreamState.STARTING)
            acquireLocks()
            val eng = WebRtcEngine(applicationContext, this)
            engine = eng
            eng.setMicEnabled(settings.micEnabled)
            eng.setBitrateKbps(settings.bitrateKbps)
            val mode = eng.startCamera(settings.facing, settings.width, settings.height, settings.fps)
            lastRequestedMode = Triple(settings.width, settings.height, settings.fps)

            repo.ui.update {
                it.copy(
                    deviceId = settings.deviceId, deviceName = settings.deviceName,
                    cameraActive = true, micEnabled = settings.micEnabled,
                    facing = eng.currentFacing, resolution = mode.label
                )
            }
            repo.engine.value = eng          // UI can now attach a preview surface
            setState(StreamState.CAMERA_ACTIVE)

            val srv = SignalingServer(this)
            server = srv
            srv.start()                      // -> onServerStarted -> mDNS -> DISCOVERABLE
        } catch (e: Exception) {
            Log.e(TAG, "start failed", e)
            stopAll(StreamState.ERROR, e.message ?: "Failed to start")
        }
    }

    // ----------------------------------------------- SignalingServer.Callbacks

    override fun onServerStarted(port: Int) {
        executor.execute {
            serverPort = port
            repo.ui.update { it.copy(signalingAddress = localIpv4()?.let { ip -> "ws://$ip:$port" }) }
            registerNsd(port)
        }
    }

    override fun onServerError(e: Exception) {
        executor.execute { stopAll(StreamState.ERROR, "Signaling server error: ${e.message}") }
    }

    override fun onClientConnected(remote: String) {
        executor.execute {
            val eng = engine ?: return@execute
            Log.i(TAG, "Desktop connected from $remote")
            peerConnected = false
            mediaFlowing = false
            sessionId = UUID.randomUUID().toString()
            repo.ui.update { it.copy(desktopAddress = remote) }
            setState(StreamState.CONNECTING)
            server?.send(statusJson("hello"))
            eng.startSession()               // phone creates the SDP offer (spec §13)
        }
    }

    override fun onClientMessage(msg: JSONObject) {
        executor.execute {
            val sid = msg.optString("sessionId")
            if (sid.isNotEmpty() && sid != sessionId) return@execute   // stale session
            try {
                when (msg.optString("type")) {
                    "answer" -> engine?.setRemoteAnswer(msg.getString("sdp"))
                    "candidate" -> {
                        val cand = msg.optString("candidate")
                        if (cand.isNotEmpty()) {
                            engine?.addRemoteCandidate(
                                msg.optString("sdpMid").ifEmpty { null },
                                msg.optInt("sdpMLineIndex", 0), cand
                            )
                        }
                    }
                    "command" -> handleCommand(msg)
                    "bye" -> endSession(clientInitiated = true)
                    "ping" -> server?.send(JSONObject().put("type", "pong"))
                }
            } catch (e: Exception) {
                Log.w(TAG, "message handling failed", e)
                sendError(e.message ?: "bad message")
            }
        }
    }

    override fun onClientDisconnected() {
        executor.execute {
            repo.ui.update { it.copy(desktopAddress = null) }
            // A live PeerConnection may outlive its signaling socket (media is
            // independent); it is torn down by ICE failure. If never connected, drop it.
            if (!peerConnected) endSession(clientInitiated = false)
        }
    }

    // -------------------------------------------------- WebRtcEngine.Listener

    override fun onLocalDescription(sdp: SessionDescription) {
        server?.send(
            JSONObject().put("type", "offer").put("sessionId", sessionId).put("sdp", sdp.description)
        )
    }

    override fun onLocalIceCandidate(candidate: IceCandidate) {
        server?.send(
            JSONObject().put("type", "candidate").put("sessionId", sessionId)
                .put("sdpMid", candidate.sdpMid)
                .put("sdpMLineIndex", candidate.sdpMLineIndex)
                .put("candidate", candidate.sdp)
        )
    }

    override fun onPeerState(state: PeerConnection.PeerConnectionState) {
        executor.execute {
            when (state) {
                PeerConnection.PeerConnectionState.NEW,
                PeerConnection.PeerConnectionState.CONNECTING -> setState(StreamState.CONNECTING)
                PeerConnection.PeerConnectionState.CONNECTED -> {
                    peerConnected = true
                    setState(if (mediaFlowing) StreamState.STREAMING else StreamState.CONNECTED)
                }
                PeerConnection.PeerConnectionState.DISCONNECTED -> {
                    peerConnected = false
                    setState(StreamState.CONNECTING)     // ICE may still recover
                }
                PeerConnection.PeerConnectionState.FAILED -> {
                    server?.send(JSONObject().put("type", "disconnected").put("reason", "connection_failed"))
                    endSession(clientInitiated = false)
                }
                PeerConnection.PeerConnectionState.CLOSED -> {}
            }
        }
    }

    override fun onMediaFlowChanged(flowing: Boolean) {
        executor.execute {
            mediaFlowing = flowing
            if (peerConnected) setState(if (flowing) StreamState.STREAMING else StreamState.CONNECTED)
        }
    }

    override fun onEngineError(message: String) {
        executor.execute {
            Log.w(TAG, message)
            sendError(message)
            if (engine?.isCameraRunning == false) stopAll(StreamState.ERROR, message)
        }
    }

    // ------------------------------------------------------ session control

    private fun endSession(clientInitiated: Boolean) {
        if (stopped) return
        setState(StreamState.DISCONNECTING)
        engine?.closeSession()
        peerConnected = false
        mediaFlowing = false
        if (clientInitiated) server?.disconnectClient()
        setState(StreamState.DISCOVERABLE)           // camera stays on, still discoverable
    }

    // -------------------------------------------------------------- commands

    private fun handleCommand(msg: JSONObject) {
        try {
            when (msg.optString("action")) {
                "setMic" -> doSetMic(msg.optBoolean("enabled", true))
                "setCamera" ->
                    doSetFacing(if (msg.optString("facing").equals("front", true)) CameraFacing.FRONT else CameraFacing.BACK)
                "setBitrate" -> {
                    settings.bitrateKbps = msg.getInt("kbps")
                    engine?.setBitrateKbps(settings.bitrateKbps)
                }
                "setCaptureMode" -> {
                    settings.width = msg.getInt("width"); settings.height = msg.getInt("height")
                    settings.fps = msg.getInt("fps")
                    doApplySettings()
                }
                "rename" -> {
                    settings.deviceName = msg.getString("name")
                    doApplySettings()
                }
                "disconnect" -> { endSession(clientInitiated = true); return }
                "getStatus" -> {}
                else -> { sendError("unknown command: ${msg.optString("action")}"); return }
            }
            sendStatus()
        } catch (e: Exception) {
            sendError(e.message ?: "command failed")
        }
    }

    private fun doSetMic(enabled: Boolean) {
        settings.micEnabled = enabled
        engine?.setMicEnabled(enabled)
        repo.ui.update { it.copy(micEnabled = enabled) }
    }

    private fun doSetFacing(facing: CameraFacing) {
        val eng = engine ?: return
        try {
            val mode = eng.startCamera(facing, settings.width, settings.height, settings.fps)
            settings.facing = facing
            repo.ui.update { it.copy(facing = facing, resolution = mode.label) }
        } catch (e: Exception) {
            sendError(e.message ?: "camera switch failed")
            if (!eng.isCameraRunning) stopAll(StreamState.ERROR, e.message)
        }
    }

    /** Re-reads settings: name, bitrate and capture mode. Only restarts capture if the request changed. */
    private fun doApplySettings() {
        val eng = engine ?: return
        repo.ui.update { it.copy(deviceName = settings.deviceName) }
        eng.setBitrateKbps(settings.bitrateKbps)
        val requested = Triple(settings.width, settings.height, settings.fps)
        if (requested != lastRequestedMode) {
            try {
                val mode = eng.startCamera(eng.currentFacing, requested.first, requested.second, requested.third)
                lastRequestedMode = requested
                repo.ui.update { it.copy(resolution = mode.label) }
            } catch (e: Exception) {
                sendError(e.message ?: "could not apply capture mode")
            }
        }
        if (serverPort != 0) registerNsd(serverPort)   // refresh advertised name
    }

    // ---------------------------------------------------------------- status

    private fun statusJson(type: String): JSONObject {
        val ui = repo.ui.value
        val mode = engine?.currentMode
        return JSONObject().apply {
            put("type", type)
            put("protocol", PROTOCOL_VERSION)
            put("sessionId", sessionId)
            put("deviceId", settings.deviceId)
            put("name", settings.deviceName)
            put("state", ui.state.name)
            put("camera", ui.facing.name.lowercase())
            put("micEnabled", ui.micEnabled)
            put("bitrateKbps", settings.bitrateKbps)
            if (mode != null) {
                put("width", mode.width); put("height", mode.height); put("fps", mode.fps)
            }
        }
    }

    private fun sendStatus() { server?.send(statusJson("status")) }

    private fun sendError(message: String) {
        server?.send(JSONObject().put("type", "error").put("message", message))
    }

    private fun setState(s: StreamState, error: String? = null) {
        repo.ui.update { it.copy(state = s, error = error ?: it.error.takeIf { _ -> s == StreamState.ERROR }) }
        if (!stopped) refreshNotification()
    }

    // ------------------------------------------------------------------ mDNS

    private fun registerNsd(port: Int) {
        unregisterNsd()
        val mgr = getSystemService(NSD_SERVICE) as NsdManager
        nsd = mgr
        val info = NsdServiceInfo().apply {
            serviceName = settings.deviceId
            serviceType = "_phonecam._tcp."
            setPort(port)
            setAttribute("id", settings.deviceId)
            setAttribute("name", settings.deviceName)
            setAttribute("proto", PROTOCOL_VERSION.toString())
            setAttribute("path", "/")
        }
        val l = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(i: NsdServiceInfo?) {
                executor.execute {
                    if (repo.ui.value.state == StreamState.CAMERA_ACTIVE) setState(StreamState.DISCOVERABLE)
                }
            }
            override fun onRegistrationFailed(i: NsdServiceInfo?, errorCode: Int) {
                executor.execute { stopAll(StreamState.ERROR, "mDNS registration failed (code $errorCode)") }
            }
            override fun onServiceUnregistered(i: NsdServiceInfo?) {}
            override fun onUnregistrationFailed(i: NsdServiceInfo?, errorCode: Int) {}
        }
        nsdListener = l
        mgr.registerService(info, NsdManager.PROTOCOL_DNS_SD, l)
    }

    private fun unregisterNsd() {
        val l = nsdListener ?: return
        runCatching { nsd?.unregisterService(l) }
        nsdListener = null
    }

    // ------------------------------------------------------------------ stop

    private fun stopAll(finalState: StreamState, error: String?) {
        if (stopped) return
        stopped = true
        setStateRaw(StreamState.STOPPING)
        runCatching { engine?.closeSession() }
        runCatching { server?.stop(500) }
        server = null
        unregisterNsd()
        val eng = engine
        engine = null
        repo.engine.value = null            // UI detaches preview
        runCatching { eng?.dispose() }
        releaseLocks()
        repo.ui.update {
            it.copy(
                state = finalState, error = error, cameraActive = false, resolution = "",
                signalingAddress = null, desktopAddress = null
            )
        }
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun setStateRaw(s: StreamState) { repo.ui.update { it.copy(state = s) } }

    // ----------------------------------------------------------------- locks

    private fun acquireLocks() {
        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        multicastLock = wm.createMulticastLock("phonecam-mdns").apply { setReferenceCounted(false); acquire() }
        @Suppress("DEPRECATION")
        val mode = if (Build.VERSION.SDK_INT >= 29) WifiManager.WIFI_MODE_FULL_LOW_LATENCY
        else WifiManager.WIFI_MODE_FULL_HIGH_PERF
        wifiLock = wm.createWifiLock(mode, "phonecam-wifi").apply { setReferenceCounted(false); acquire() }
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "phonecam:stream")
            .apply { setReferenceCounted(false); acquire() }
    }

    private fun releaseLocks() {
        runCatching { multicastLock?.takeIf { it.isHeld }?.release() }
        runCatching { wifiLock?.takeIf { it.isHeld }?.release() }
        runCatching { wakeLock?.takeIf { it.isHeld }?.release() }
        multicastLock = null; wifiLock = null; wakeLock = null
    }

    // ---------------------------------------------------------- notification

    private fun statusText(): String = when (repo.ui.value.state) {
        StreamState.STREAMING -> "Streaming to Desktop"
        StreamState.CONNECTED -> "Connected to Desktop"
        StreamState.CONNECTING -> "Connecting to Desktop"
        StreamState.DISCOVERABLE -> "Waiting for Desktop"
        StreamState.STOPPING -> "Stopping"
        else -> "Starting"
    }

    private fun buildNotification(): Notification {
        val open = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val stop = PendingIntent.getService(
            this, 1,
            Intent(this, StreamingService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentTitle("PhoneCamStream is using your camera and microphone")
            .setContentText(statusText())
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setContentIntent(open)
            .addAction(0, "Stop", stop)
            .build()
    }

    private fun refreshNotification() {
        (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).notify(NOTIF_ID, buildNotification())
    }

    private fun localIpv4(): String? = runCatching {
        NetworkInterface.getNetworkInterfaces().toList()
            .filter { it.isUp && !it.isLoopback }
            .flatMap { it.inetAddresses.toList() }
            .firstOrNull { it is Inet4Address && it.isSiteLocalAddress }
            ?.hostAddress
    }.getOrNull()

    companion object {
        private const val TAG = "StreamingService"
        const val PROTOCOL_VERSION = 1
        private const val CHANNEL_ID = "phonecam_stream"
        private const val NOTIF_ID = 1001

        const val ACTION_START = "com.phonecam.stream.START"
        const val ACTION_STOP = "com.phonecam.stream.STOP"
        const val ACTION_SET_MIC = "com.phonecam.stream.SET_MIC"
        const val ACTION_SET_FACING = "com.phonecam.stream.SET_FACING"
        const val ACTION_APPLY_SETTINGS = "com.phonecam.stream.APPLY_SETTINGS"
        const val EXTRA_ENABLED = "enabled"
        const val EXTRA_FACING = "facing"

        /** Must be called while the app is visible (Android 12+ background-start limits). */
        fun start(ctx: Context) {
            ContextCompat.startForegroundService(
                ctx, Intent(ctx, StreamingService::class.java).setAction(ACTION_START)
            )
        }

        fun stop(ctx: Context) = command(ctx, ACTION_STOP)

        fun command(ctx: Context, action: String, extras: Intent.() -> Unit = {}) {
            ctx.startService(Intent(ctx, StreamingService::class.java).setAction(action).apply(extras))
        }
    }
}