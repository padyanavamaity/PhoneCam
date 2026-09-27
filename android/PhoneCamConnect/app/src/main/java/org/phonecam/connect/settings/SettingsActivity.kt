package org.phonecam.connect.settings

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Bundle
import android.os.IBinder
import android.util.Log
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import androidx.activity.ComponentActivity
import org.phonecam.connect.R
import org.phonecam.connect.StreamingService
import org.phonecam.connect.stream.CameraConfigRepository
import org.phonecam.connect.stream.CameraFacing
import org.phonecam.connect.stream.CameraStreamConfig
import org.phonecam.connect.stream.SharedPrefsConfigStore
import org.phonecam.connect.stream.SizeFps
import org.phonecam.connect.stream.VideoCodec
import org.phonecam.connect.stream.WebRtcEngine

/**
 * In-app settings screen (plan §6).
 *
 * Framework widgets only (project has no appcompat). Values load/save via
 * [CameraConfigRepository]; changes apply immediately — live camera params
 * are pushed to the bound [StreamingService] (resolution/FPS/facing), and
 * stream params (bitrate/codec) are persisted for the next session start.
 */
class SettingsActivity : ComponentActivity() {

    private val TAG = "SettingsActivity"

    private lateinit var repository: CameraConfigRepository

    private var streamingService: StreamingService? = null
    private var isBound = false

    // Views
    private lateinit var spinnerFacing: Spinner
    private lateinit var spinnerResolution: Spinner
    private lateinit var spinnerFps: Spinner
    private lateinit var spinnerCodec: Spinner
    private lateinit var seekBitrate: SeekBar
    private lateinit var textBitrateValue: TextView
    private lateinit var textBackCaps: TextView
    private lateinit var textFrontCaps: TextView
    private lateinit var buttonBack: TextView

    // Available capabilities (from Camera2Enumerator — real hardware, no fakes)
    private var backFormats: List<SizeFps> = emptyList()
    private var frontFormats: List<SizeFps> = emptyList()

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            val binder = service as StreamingService.LocalBinder
            streamingService = binder.getService()
            isBound = true
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            streamingService = null
            isBound = false
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)

        repository = CameraConfigRepository(SharedPrefsConfigStore(this)) {
            Log.d(TAG, it)
        }

        bindViews()
        enumerateCapabilities()
        loadIntoUi()
        wireListeners()

        // Settings label
        findViewById<TextView>(R.id.settingsSubNote)?.text = getString(R.string.settings_note_stream_apply)

        // Back navigation — returns to MainActivity without touching the service.
        buttonBack.setOnClickListener {
            finish()
        }
    }

    override fun onStart() {
        super.onStart()
        Intent(this, StreamingService::class.java).also { bindService(it, serviceConnection, Context.BIND_AUTO_CREATE) }
    }

    override fun onStop() {
        super.onStop()
        if (isBound) {
            unbindService(serviceConnection)
            isBound = false
        }
    }

    private fun bindViews() {
        spinnerFacing = findViewById(R.id.spinnerFacing)
        spinnerResolution = findViewById(R.id.spinnerResolution)
        spinnerFps = findViewById(R.id.spinnerFps)
        spinnerCodec = findViewById(R.id.spinnerCodec)
        seekBitrate = findViewById(R.id.seekBitrate)
        textBitrateValue = findViewById(R.id.textBitrateValue)
        textBackCaps = findViewById(R.id.textBackCaps)
        textFrontCaps = findViewById(R.id.textFrontCaps)
        buttonBack = findViewById(R.id.buttonBack)
    }

    private fun enumerateCapabilities() {
        // Enumerate directly via Engine helper — no camera open, no permission needed.
        val engineProbe = WebRtcEngine(this, object : WebRtcEngine.EngineEvents {
            override fun onCameraOpened() {}
            override fun onCameraFailed(message: String) {}
            override fun onCameraSwitched(isFront: Boolean) {}
            override fun onMeasuredStats(width: Int, height: Int, fps: Int) {}
        })

        backFormats = engineProbe.supportedFormats(CameraFacing.BACK)
        frontFormats = engineProbe.supportedFormats(CameraFacing.FRONT)

        textBackCaps.text = summarize("Back", backFormats)
        textFrontCaps.text = summarize("Front", frontFormats)
    }

    private fun summarize(label: String, formats: List<SizeFps>): String {
        if (formats.isEmpty()) return "$label: no data"
        val top = formats.take(6).joinToString(", ") { "${it.width}x${it.height}@${it.fps}" }
        return "$label: $top" + if (formats.size > 6) " …" else ""
    }

    private fun loadIntoUi() {
        val cfg = repository.load(supportedForFacing(currentFacingSelection()))

        // Facing
        val facingItems = listOf("Back", "Front")
        spinnerFacing.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, facingItems)
        spinnerFacing.setSelection(if (cfg.facing == CameraFacing.FRONT) 1 else 0)

        // Resolution / FPS lists from SUPPORTED formats only (never invent).
        val formats = supportedForFacing(cfg.facing)
        val resolutions = formats.map { "${it.width}x${it.height}" }.distinct().ifEmpty {
            listOf("${cfg.width}x${cfg.height}")
        }
        val resAdapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, resolutions)
        spinnerResolution.adapter = resAdapter
        val selResIdx = resolutions.indexOf("${cfg.width}x${cfg.height}").coerceAtLeast(0)
        spinnerResolution.setSelection(selResIdx)

        val currentSize = formats.getOrNull(selResIdx)
        val fpsOptions = formats.filter { it.width == currentSize?.width && it.height == currentSize?.height }
            .map { it.fps }
            .distinct()
            .sorted()
            .ifEmpty { listOf(cfg.fps) }
        spinnerFps.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, fpsOptions.map { "$it fps" })
        val selFpsIdx = fpsOptions.indexOf(cfg.fps).coerceAtLeast(0)
        spinnerFps.setSelection(selFpsIdx)

        // Codec
        val codecItems = VideoCodec.entries.map { it.name }
        spinnerCodec.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, codecItems)
        spinnerCodec.setSelection(VideoCodec.entries.indexOf(cfg.codec).coerceAtLeast(0))

        // Bitrate — 0.5 Mbps steps, 0.5..16 Mbps
        seekBitrate.max = BITRATE_STEPS - 1
        val step = bitrateToStep(cfg.videoBitrateBps)
        seekBitrate.progress = step
        textBitrateValue.text = formatBitrate(stepToBitrate(step))
    }

    private fun wireListeners() {
        // Facing live-applies switchCamera when camera active; otherwise persists.
        spinnerFacing.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                val facing = if (position == 1) CameraFacing.FRONT else CameraFacing.BACK
                val cfg = repository.load().copy(facing = facing)
                repository.save(cfg)
                streamingService?.applyConfig(cfg)   // live-apply if camera active
                refreshResolutionAndFps(facing)
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        spinnerResolution.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                val facing = currentFacingSelection()
                val formats = supportedForFacing(facing)
                val resolutions = formats.map { "${it.width}x${it.height}" }.distinct()
                val sel = resolutions.getOrNull(position) ?: return
                val parts = sel.split("x")
                if (parts.size == 2) {
                    val w = parts[0].toIntOrNull() ?: return
                    val h = parts[1].toIntOrNull() ?: return
                    val fps = parseFps(spinnerFps.selectedItem as? String) ?: 30
                    val cfg = repository.load().copy(width = w, height = h, fps = fps)
                    repository.save(cfg)
                    streamingService?.applyConfig(cfg)
                    refreshFpsForSize(facing, w, h, fps)
                }
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        spinnerFps.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                val fps = parseFps(spinnerFps.selectedItem as? String) ?: return
                val cfg = repository.load().copy(fps = fps)
                repository.save(cfg)
                streamingService?.applyConfig(cfg)
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        spinnerCodec.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                val codec = VideoCodec.entries.getOrNull(position) ?: VideoCodec.VP8
                val cfg = repository.load().copy(codec = codec)
                repository.save(cfg)
                // Not applied live — codec is an RTP-level property applied at session start.
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        seekBitrate.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                if (!fromUser) return
                val bitrate = stepToBitrate(progress)
                textBitrateValue.text = formatBitrate(bitrate)
                val cfg = repository.load().copy(videoBitrateBps = bitrate)
                repository.save(cfg)
                // Not applied live — bitrate is an RTP-level property.
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })
    }

    private fun refreshResolutionAndFps(facing: CameraFacing) {
        val formats = supportedForFacing(facing)
        val cfg = repository.load()
        val resolutions = formats.map { "${it.width}x${it.height}" }.distinct().ifEmpty {
            listOf("${cfg.width}x${cfg.height}")
        }
        spinnerResolution.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, resolutions)
        val selResIdx = resolutions.indexOf("${cfg.width}x${cfg.height}").coerceAtLeast(0)
        spinnerResolution.setSelection(selResIdx)
        val size = formats.getOrNull(selResIdx)
        if (size != null) refreshFpsForSize(facing, size.width, size.height, cfg.fps)
    }

    private fun refreshFpsForSize(facing: CameraFacing, width: Int, height: Int, currentFps: Int) {
        val formats = supportedForFacing(facing)
        val fpsOptions = formats.filter { it.width == width && it.height == height }
            .map { it.fps }
            .distinct()
            .sorted()
            .ifEmpty { listOf(currentFps) }
        spinnerFps.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, fpsOptions.map { "$it fps" })
        spinnerFps.setSelection(fpsOptions.indexOf(currentFps).coerceAtLeast(0))
    }

    private fun supportedForFacing(facing: CameraFacing): List<SizeFps> =
        if (facing == CameraFacing.FRONT) frontFormats else backFormats

    private fun currentFacingSelection(): CameraFacing =
        if (spinnerFacing.selectedItemPosition == 1) CameraFacing.FRONT else CameraFacing.BACK

    private fun parseFps(text: String?): Int? = text?.removeSuffix(" fps")?.toIntOrNull()

    // Bitrate helpers: steps of 0.5 Mbps between 0.5 and 16 inclusive.
    private fun stepToBitrate(step: Int): Int {
        val mbp = 0.5f + 0.5f * step
        return (mbp * 1_000_000).toInt()
    }

    private fun bitrateToStep(bps: Int): Int {
        val mbp = bps.coerceIn(CameraStreamConfig.MIN_BITRATE_BPS, CameraStreamConfig.MAX_BITRATE_BPS) / 1_000_000f
        val step = ((mbp - 0.5f) / 0.5f).toInt()
        return step.coerceIn(0, BITRATE_STEPS - 1)
    }

    private fun formatBitrate(bps: Int): String =
        if (bps >= 1_000_000) "%.1f Mbps".format(bps / 1_000_000f) else "${bps / 1000} kbps"

    companion object {
        private const val BITRATE_STEPS = 32 // (16.0 - 0.5) / 0.5 + 1
    }
}
