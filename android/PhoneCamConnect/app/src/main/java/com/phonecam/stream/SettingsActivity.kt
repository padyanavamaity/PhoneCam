package com.phonecam.stream

import android.os.Bundle
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.Spinner
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SwitchCompat
import kotlin.math.abs

class SettingsActivity : AppCompatActivity() {

    private lateinit var settings: AppSettings
    private lateinit var spRes: Spinner
    private lateinit var spFps: Spinner
    private lateinit var etName: EditText
    private lateinit var etBitrate: EditText
    private lateinit var swAuto: SwitchCompat

    private var resolutions: List<Pair<Int, Int>> = emptyList()
    private var fpsOptions: List<Int> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)
        settings = AppSettings(this)

        spRes = findViewById(R.id.spRes)
        spFps = findViewById(R.id.spFps)
        etName = findViewById(R.id.etName)
        etBitrate = findViewById(R.id.etBitrate)
        swAuto = findViewById(R.id.swAuto)

        findViewById<TextView>(R.id.tvDeviceId).text = settings.deviceId
        etName.setText(if (settings.deviceName == settings.deviceId) "" else settings.deviceName)
        etBitrate.setText(settings.bitrateKbps.toString())
        swAuto.isChecked = settings.autoStart

        // Only offer modes the camera actually reports (spec §4).
        resolutions = WebRtcEngine.supportedResolutions(this, settings.facing).ifEmpty { listOf(1280 to 720) }
        spRes.adapter = ArrayAdapter(
            this, android.R.layout.simple_spinner_dropdown_item,
            resolutions.map { "${it.first}×${it.second}" }
        )
        val target = settings.width * settings.height
        val idx = resolutions.indices.minByOrNull { abs(resolutions[it].first * resolutions[it].second - target) } ?: 0
        spRes.setSelection(idx)
        refreshFps(resolutions[idx], settings.fps)

        spRes.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) {
                val current = fpsOptions.getOrNull(spFps.selectedItemPosition) ?: settings.fps
                refreshFps(resolutions[pos], current)
            }
            override fun onNothingSelected(p: AdapterView<*>?) {}
        }

        findViewById<Button>(R.id.btnSave).setOnClickListener { save() }
        renderDiagnostics()
    }

    private fun refreshFps(res: Pair<Int, Int>, preferred: Int) {
        val max = WebRtcEngine.maxFps(this, settings.facing, res.first, res.second)
        fpsOptions = listOf(15, 24, 30, 60).filter { it <= max }.ifEmpty { listOf(max) }
        spFps.adapter = ArrayAdapter(
            this, android.R.layout.simple_spinner_dropdown_item, fpsOptions.map { "$it FPS" }
        )
        val i = fpsOptions.indices.minByOrNull { abs(fpsOptions[it] - preferred) } ?: 0
        spFps.setSelection(i)
    }

    private fun save() {
        val res = resolutions[spRes.selectedItemPosition]
        settings.width = res.first
        settings.height = res.second
        settings.fps = fpsOptions.getOrElse(spFps.selectedItemPosition) { 30 }
        settings.bitrateKbps = etBitrate.text.toString().toIntOrNull() ?: settings.bitrateKbps
        settings.autoStart = swAuto.isChecked
        settings.deviceName = etName.text.toString().ifBlank { settings.deviceId }

        if (StreamingRepository.ui.value.isRunning) {
            StreamingService.command(this, StreamingService.ACTION_APPLY_SETTINGS)
        }
        finish()
    }

    private fun renderDiagnostics() {
        val ui = StreamingRepository.ui.value
        val version = runCatching { packageManager.getPackageInfo(packageName, 0).versionName }.getOrNull()
        findViewById<TextView>(R.id.tvDiag).text = buildString {
            appendLine("Version:    $version")
            appendLine("State:      ${ui.state}")
            appendLine("Camera:     ${if (ui.cameraActive) ui.resolution else "off"}")
            appendLine("Signaling:  ${ui.signalingAddress ?: "-"}")
            appendLine("Desktop:    ${ui.desktopAddress ?: "-"}")
            append("Last error: ${ui.error ?: "-"}")
        }
    }
}