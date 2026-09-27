package org.phonecam.connect

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.provider.Settings
import android.util.Log
import android.view.View
import android.widget.Button
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import org.phonecam.connect.settings.SettingsActivity
import org.phonecam.connect.stream.ServiceState
import org.phonecam.connect.stream.StreamPhase
import org.webrtc.RendererCommon
import org.webrtc.SurfaceViewRenderer

class MainActivity : ComponentActivity() {

    companion object {
        private const val TAG = "MainActivity"
        private const val REQ_PERMISSIONS = 1001
    }

    // Views
    private lateinit var buttonStream: Button
    private lateinit var buttonSettings: TextView
    private lateinit var connectionStatusDot: View
    private lateinit var connectionStatusText: TextView
    private lateinit var textDesktopValue: TextView
    private lateinit var textIpValue: TextView
    private lateinit var textNetworkValue: TextView
    private lateinit var textLatencyValue: TextView
    private lateinit var textPairingValue: TextView
    private lateinit var textRes: TextView
    private lateinit var textFps: TextView
    private lateinit var textBitrate: TextView
    private lateinit var textCodec: TextView
    private lateinit var previewPlaceholder: TextView
    private lateinit var errorBanner: TextView
    private lateinit var permCamera: TextView
    private lateinit var permMic: TextView
    private lateinit var statusWebrtc: TextView
    private lateinit var statusStreaming: TextView
    private lateinit var previewView: SurfaceViewRenderer

    // Service binding
    private var streamingService: StreamingService? = null
    private var isBound = false
    private var previewAttached = false

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            val binder = service as StreamingService.LocalBinder
            streamingService = binder.getService()
            isBound = true
            Log.d(TAG, "Service connected")

            observeServiceState()
            observePreviewReady()

            // StateFlow immediately emits the CURRENT value — so a late-bound
            // Activity sees CAMERA_ACTIVE etc. without waiting for a transition.
            // If the service is already camera-active, attach preview now.
            attemptPreviewAttach()
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            Log.d(TAG, "Service disconnected")
            streamingService?.detachPreview()
            streamingService = null
            isBound = false
            previewAttached = false
        }
    }

    private val requiredPermissions = mutableListOf(
        Manifest.permission.CAMERA,
        Manifest.permission.RECORD_AUDIO
    ).also {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            it.add(Manifest.permission.POST_NOTIFICATIONS)
        }
    }.toTypedArray()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        bindViews()
        setupListeners()
        checkPermissions()
    }

    override fun onStart() {
        super.onStart()
        Intent(this, StreamingService::class.java).also { intent ->
            bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE)
        }
    }

    override fun onStop() {
        super.onStop()
        if (isBound) {
            streamingService?.detachPreview()
            unbindService(serviceConnection)
            isBound = false
            previewAttached = false
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        // Release the renderer; the EGL context is owned by the service.
        if (::previewView.isInitialized) {
            previewView.release()
        }
    }

    private fun observeServiceState() {
        lifecycleScope.launch {
            streamingService?.serviceState?.collectLatest { state ->
                updateUi(state)
            }
        }
    }

    private fun observePreviewReady() {
        lifecycleScope.launch {
            streamingService?.previewReady?.collectLatest { ready ->
                Log.d(TAG, "Preview ready state changed: $ready")
                if (ready && !previewAttached) {
                    attemptPreviewAttach()
                }
            }
        }
    }

    private fun attemptPreviewAttach() {
        if (previewAttached) {
            Log.d(TAG, "Preview already attached, skipping")
            return
        }

        val service = streamingService
        if (service == null) {
            Log.w(TAG, "Cannot attach preview: service not bound")
            return
        }

        if (!::previewView.isInitialized) {
            Log.w(TAG, "Cannot attach preview: previewView not initialized")
            return
        }

        Log.d(TAG, "Attempting to attach preview...")
        val attached = service.attachPreview(previewView)
        if (attached) {
            previewAttached = true
            previewView.visibility = View.VISIBLE
            Log.d(TAG, "Preview attached successfully")
        } else {
            Log.d(TAG, "Preview attachment deferred — waiting for EGL initialization")
        }
    }

    private fun bindViews() {
        buttonStream = findViewById(R.id.buttonStream)
        buttonSettings = findViewById(R.id.buttonSettings)
        connectionStatusDot = findViewById(R.id.connectionStatusDot)
        connectionStatusText = findViewById(R.id.connectionStatusText)
        textDesktopValue = findViewById(R.id.textDesktopValue)
        textIpValue = findViewById(R.id.textIpValue)
        textNetworkValue = findViewById(R.id.textNetworkValue)
        textLatencyValue = findViewById(R.id.textLatencyValue)
        textPairingValue = findViewById(R.id.textPairingValue)
        textRes = findViewById(R.id.textRes)
        textFps = findViewById(R.id.textFps)
        textBitrate = findViewById(R.id.textBitrate)
        textCodec = findViewById(R.id.textCodec)
        previewPlaceholder = findViewById(R.id.previewPlaceholder)
        errorBanner = findViewById(R.id.errorBanner)
        permCamera = findViewById(R.id.permCamera)
        permMic = findViewById(R.id.permMic)
        statusWebrtc = findViewById(R.id.statusWebrtc)
        statusStreaming = findViewById(R.id.statusStreaming)
        previewView = findViewById(R.id.previewView)
    }

    private fun setupListeners() {
        buttonStream.setOnClickListener {
            if (!isBound) {
                Log.w(TAG, "Service not bound")
                showError(getString(R.string.error_service))
                return@setOnClickListener
            }

            val state = streamingService?.serviceState?.value
            when (state?.phase) {
                StreamPhase.IDLE, StreamPhase.ERROR, StreamPhase.STOPPING -> {
                    if (!hasAllPermissions()) {
                        showError(getString(R.string.error_permissions_required))
                        requestPermissions()
                        return@setOnClickListener
                    }
                    streamingService?.startStreaming()
                }
                else -> streamingService?.stopStreaming()
            }
        }

        buttonSettings.setOnClickListener {
            Log.d(TAG, "Settings clicked")
            startActivity(Intent(this, SettingsActivity::class.java))
        }

        errorBanner.setOnClickListener {
            if (errorBanner.text.toString().contains(getString(R.string.action_open_settings))) {
                openAppSettings()
            }
        }
    }

    // ------------------------------------------------------------------
    // Phase-driven UI (plan §7)
    // ------------------------------------------------------------------

    private fun updateUi(state: ServiceState) {
        Log.d(TAG, "updateUi: phase=${state.phase} err=${state.errorMessage}")

        // Status text + dot — distinguishes local camera from actual streaming.
        val (statusText, dotRes) = when (state.phase) {
            StreamPhase.IDLE -> getString(R.string.status_idle) to R.drawable.status_badge_inactive
            StreamPhase.STARTING_CAMERA -> getString(R.string.status_starting_camera) to R.drawable.bg_status_badge_warn
            StreamPhase.CAMERA_ACTIVE, StreamPhase.WEBRTC_READY ->
                getString(R.string.status_camera_active_local) to R.drawable.status_badge_active
            StreamPhase.CONNECTING -> getString(R.string.status_connecting) to R.drawable.bg_status_badge_warn
            StreamPhase.CONNECTED, StreamPhase.STREAMING ->
                getString(R.string.status_camera_active_streaming) to R.drawable.status_badge_active
            StreamPhase.STOPPING -> getString(R.string.status_stopping) to R.drawable.bg_status_badge_warn
            StreamPhase.ERROR -> getString(R.string.status_error) to R.drawable.bg_status_badge_error
        }
        connectionStatusText.text = statusText
        connectionStatusDot.background = ContextCompat.getDrawable(this, dotRes)

        // Primary button — direct mapping from StreamPhase; one source of truth.
        buttonStream.isEnabled = when (state.phase) {
            StreamPhase.IDLE, StreamPhase.ERROR,
            StreamPhase.CAMERA_ACTIVE, StreamPhase.WEBRTC_READY,
            StreamPhase.CONNECTING, StreamPhase.CONNECTED, StreamPhase.STREAMING -> true
            // Double-tap guard: ignore clicks while a transition is in flight.
            StreamPhase.STARTING_CAMERA, StreamPhase.STOPPING -> false
        }

        buttonStream.text = when (state.phase) {
            StreamPhase.IDLE, StreamPhase.ERROR -> getString(R.string.action_start_stream)
            StreamPhase.STARTING_CAMERA -> getString(R.string.status_starting_camera)
            StreamPhase.STOPPING -> getString(R.string.status_stopping)
            else -> getString(R.string.action_stop_stream)
        }

        // Error banner
        if (state.errorMessage != null) {
            errorBanner.text = state.errorMessage
            errorBanner.visibility = View.VISIBLE
        } else {
            errorBanner.visibility = View.GONE
        }

        // Preview placeholder
        previewPlaceholder.visibility = if (state.cameraActive) View.GONE else View.VISIBLE

        // Stats — honest: measured when camera active, "—" otherwise.
        if (state.cameraActive && state.measuredWidth > 0) {
            textRes.text = "${state.measuredWidth}x${state.measuredHeight}"
            textFps.text = "${state.measuredFps} FPS"
        } else {
            textRes.text = getString(R.string.value_unknown)
            textFps.text = getString(R.string.value_unknown)
        }

        // Bitrate/codec are RTP-level: not measurable until signaling.
        textBitrate.text = getString(R.string.value_unknown)
        textCodec.text = getString(R.string.value_unknown)

        // Statuses
        statusWebrtc.text = when (state.phase) {
            StreamPhase.WEBRTC_READY, StreamPhase.CONNECTING,
            StreamPhase.CONNECTED, StreamPhase.STREAMING -> getString(R.string.state_active)
            else -> getString(R.string.state_idle)
        }
        statusStreaming.text = if (state.phase == StreamPhase.STREAMING)
            getString(R.string.state_active) else getString(R.string.state_off)

        // Connection card (mostly placeholder until signaling exists)
        textDesktopValue.text = getString(R.string.value_unknown)
        textIpValue.text = getLocalIpAddress() ?: getString(R.string.value_unknown)
        textNetworkValue.text = getString(R.string.value_unknown)
        textLatencyValue.text = getString(R.string.value_unknown)
        textPairingValue.text = getString(R.string.value_no_pairing)
    }

    private fun checkPermissions() {
        val cameraGranted = ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        val micGranted = ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

        permCamera.text = if (cameraGranted) getString(R.string.state_granted) else getString(R.string.state_required)
        permCamera.setTextColor(ContextCompat.getColor(this, if (cameraGranted) R.color.pc_success else R.color.pc_warning))

        permMic.text = if (micGranted) getString(R.string.state_granted) else getString(R.string.state_required)
        permMic.setTextColor(ContextCompat.getColor(this, if (micGranted) R.color.pc_success else R.color.pc_warning))

        if (!cameraGranted || !micGranted) {
            requestPermissions()
        }
    }

    private fun requestPermissions() {
        ActivityCompat.requestPermissions(this, requiredPermissions, REQ_PERMISSIONS)
    }

    private fun hasAllPermissions(): Boolean {
        val cameraGranted = ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        val micGranted = ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
        return cameraGranted && micGranted
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<String>, grantResults: IntArray, deviceId: Int) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults, deviceId)
        if (requestCode == REQ_PERMISSIONS) {
            var allGranted = true
            var permanentlyDenied = false

            for (i in permissions.indices) {
                if (grantResults[i] != PackageManager.PERMISSION_GRANTED) {
                    allGranted = false
                    if (!ActivityCompat.shouldShowRequestPermissionRationale(this, permissions[i])) {
                        permanentlyDenied = true
                    }
                }
            }

            if (permanentlyDenied) {
                showError(getString(R.string.error_permissions_permanently_denied) + " " + getString(R.string.action_open_settings))
            } else if (!allGranted) {
                showError(getString(R.string.error_permissions_required))
            } else {
                errorBanner.visibility = View.GONE
            }

            checkPermissions()
        }
    }

    private fun showError(message: String) {
        errorBanner.text = message
        errorBanner.visibility = View.VISIBLE
    }

    private fun openAppSettings() {
        Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).also { intent ->
            intent.data = Uri.fromParts("package", packageName, null)
            startActivity(intent)
        }
    }

    private fun getLocalIpAddress(): String? {
        return try {
            val interfaces = java.net.NetworkInterface.getNetworkInterfaces()
            while (interfaces.hasMoreElements()) {
                val networkInterface = interfaces.nextElement()
                val addresses = networkInterface.inetAddresses
                while (addresses.hasMoreElements()) {
                    val address = addresses.nextElement()
                    if (!address.isLoopbackAddress && address is java.net.Inet4Address) {
                        return address.hostAddress
                    }
                }
            }
            null
        } catch (e: Exception) {
            null
        }
    }
}
