package com.phonecam.stream

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.widget.Button
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SwitchCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.webrtc.RendererCommon
import org.webrtc.SurfaceViewRenderer

/**
 * UI ONLY (spec §3). It never touches the camera. It observes StreamingRepository,
 * sends commands to StreamingService, and attaches/detaches a preview surface to the
 * engine's already-running video track. Recreating this Activity does not reopen the camera.
 */
class MainActivity : AppCompatActivity() {

    private val repo = StreamingRepository
    private lateinit var settings: AppSettings

    private lateinit var preview: SurfaceViewRenderer
    private lateinit var placeholder: TextView
    private lateinit var tvOverlay: TextView
    private lateinit var tvCamera: TextView
    private lateinit var tvConnection: TextView
    private lateinit var tvDevice: TextView
    private lateinit var tvError: TextView
    private lateinit var btnToggle: Button
    private lateinit var btnCamera: Button
    private lateinit var swMic: SwitchCompat

    private var attachedEngine: WebRtcEngine? = null
    private var rendererReady = false
    private var autoStartHandled = false

    private val requiredPermissions: Array<String>
        get() = buildList {
            add(Manifest.permission.CAMERA)
            add(Manifest.permission.RECORD_AUDIO)
            if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
        }.toTypedArray()

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
            if (hasCorePermissions()) StreamingService.start(this)
            else Toast.makeText(this, "Camera and microphone permission are required", Toast.LENGTH_LONG).show()
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        settings = AppSettings(this)

        preview = findViewById(R.id.previewView)
        placeholder = findViewById(R.id.previewPlaceholder)
        tvOverlay = findViewById(R.id.tvOverlay)
        tvCamera = findViewById(R.id.tvCamera)
        tvConnection = findViewById(R.id.tvConnection)
        tvDevice = findViewById(R.id.tvDevice)
        tvError = findViewById(R.id.tvError)
        btnToggle = findViewById(R.id.btnToggle)
        btnCamera = findViewById(R.id.btnCamera)
        swMic = findViewById(R.id.swMic)

        findViewById<Button>(R.id.btnSettings).setOnClickListener {
            startActivity(Intent(this, SettingsActivity::class.java))
        }

        btnToggle.setOnClickListener {
            if (repo.ui.value.isRunning) StreamingService.stop(this)
            else if (hasCorePermissions()) StreamingService.start(this)
            else permissionLauncher.launch(requiredPermissions)
        }

        btnCamera.setOnClickListener {
            val next = repo.ui.value.facing.opposite()
            if (repo.ui.value.isRunning) {
                StreamingService.command(this, StreamingService.ACTION_SET_FACING) {
                    putExtra(StreamingService.EXTRA_FACING, next.name)
                }
            } else {
                settings.facing = next
                repo.ui.update { it.copy(facing = next) }
            }
        }

        // OnClick only fires for user taps, not for programmatic state changes.
        swMic.setOnClickListener {
            val on = swMic.isChecked
            if (repo.ui.value.isRunning) {
                StreamingService.command(this, StreamingService.ACTION_SET_MIC) {
                    putExtra(StreamingService.EXTRA_ENABLED, on)
                }
            } else {
                settings.micEnabled = on
                repo.ui.update { it.copy(micEnabled = on) }
            }
        }

        // Collectors live for the Activity; repeatOnLifecycle pauses them while stopped.
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch { repo.ui.collect { render(it) } }
                launch { repo.engine.collect { bindEngine(it) } }
            }
        }
    }

    override fun onStart() {
        super.onStart()
        syncIdleStateFromSettings()
        if (settings.autoStart && !autoStartHandled && !repo.ui.value.isRunning && hasCorePermissions()) {
            autoStartHandled = true
            StreamingService.start(this)
        }
    }

    override fun onStop() {
        // Only the preview surface goes away; the service keeps streaming.
        attachedEngine?.detachPreview(preview)
        attachedEngine = null
        if (rendererReady) { preview.release(); rendererReady = false }
        super.onStop()
    }

    private fun syncIdleStateFromSettings() {
        if (repo.ui.value.isRunning) return
        repo.ui.update {
            it.copy(
                deviceId = settings.deviceId, deviceName = settings.deviceName,
                micEnabled = settings.micEnabled, facing = settings.facing
            )
        }
    }

    private fun hasCorePermissions() =
        listOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO).all {
            ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
        }

    // ---------------------------------------------------------------- preview

    private fun bindEngine(engine: WebRtcEngine?) {
        if (engine === attachedEngine) return
        attachedEngine?.detachPreview(preview)
        if (rendererReady) { preview.release(); rendererReady = false }
        attachedEngine = engine
        if (engine != null) {
            preview.init(engine.eglBase.eglBaseContext, null)
            preview.setEnableHardwareScaler(true)
            preview.setScalingType(RendererCommon.ScalingType.SCALE_ASPECT_FIT)
            preview.setMirror(repo.ui.value.facing == CameraFacing.FRONT)
            engine.attachPreview(preview)
            rendererReady = true
        }
    }

    // ----------------------------------------------------------------- render

    private fun render(ui: UiState) {
        if (rendererReady) preview.setMirror(ui.facing == CameraFacing.FRONT)

        placeholder.visibility = if (ui.cameraActive) android.view.View.GONE else android.view.View.VISIBLE
        tvOverlay.text = if (ui.cameraActive) "${ui.deviceName}\n${ui.resolution}" else ""

        tvCamera.text = if (ui.cameraActive) dotted(GREEN, "Camera Active") else dotted(GRAY, "Camera Off")

        val (color, label) = when (ui.state) {
            StreamState.IDLE -> GRAY to "Not streaming"
            StreamState.STARTING -> AMBER to "Starting…"
            StreamState.CAMERA_ACTIVE -> AMBER to "Starting discovery…"
            StreamState.DISCOVERABLE -> AMBER to "Waiting for Desktop"
            StreamState.CONNECTING -> AMBER to "Connecting"
            StreamState.CONNECTED -> GREEN to "Connected"
            StreamState.STREAMING -> GREEN to "Streaming"
            StreamState.DISCONNECTING -> AMBER to "Disconnected"
            StreamState.STOPPING -> AMBER to "Stopping…"
            StreamState.ERROR -> RED to "Error"
        }
        tvConnection.text = dotted(color, "Connection: $label")

        tvDevice.text = if (ui.deviceName.isNotBlank() && ui.deviceName != ui.deviceId)
            "Device: ${ui.deviceName} · ${ui.deviceId}" else "Device: ${ui.deviceId}"

        tvError.visibility = if (ui.error != null) android.view.View.VISIBLE else android.view.View.GONE
        tvError.text = ui.error ?: ""

        btnToggle.isEnabled = ui.state != StreamState.STARTING && ui.state != StreamState.STOPPING
        btnToggle.text = if (ui.isRunning) "STOP CAMERA STREAM" else "START CAMERA STREAM"

        btnCamera.text = "Camera: ${if (ui.facing == CameraFacing.FRONT) "Front" else "Back"}"
        swMic.isChecked = ui.micEnabled
    }

    private fun dotted(color: Int, text: String) = SpannableStringBuilder("● $text").apply {
        setSpan(ForegroundColorSpan(color), 0, 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
    }

    companion object {
        private val GREEN = Color.parseColor("#3FB950")
        private val AMBER = Color.parseColor("#D29922")
        private val RED = Color.parseColor("#F85149")
        private val GRAY = Color.parseColor("#8B949E")
    }
}