package com.padyanava.phonecamstream

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.widget.*
import androidx.activity.ComponentActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

class MainActivity : ComponentActivity() {
    private val requestCode = 10
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val layout = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(32,48,32,32) }
        val title = TextView(this).apply { text = "PhoneCam Stream"; textSize = 28f }
        val info = TextView(this).apply { text = "Phone → Wi‑Fi → PC\n1080p30 • screen-off capable"; textSize = 16f; setPadding(0,16,0,24) }
        val url = EditText(this).apply { hint = "PC signaling URL"; setText("ws://192.168.1.100:8765") }
        val start = Button(this).apply { text = "START STREAM" }
        val stop = Button(this).apply { text = "STOP STREAM"; isEnabled = false }
        val status = TextView(this).apply { text = "Status: idle"; setPadding(0,20,0,0) }
        layout.addView(title); layout.addView(info); layout.addView(url); layout.addView(start); layout.addView(stop); layout.addView(status)
        setContentView(layout)

        start.setOnClickListener {
            if (!hasPermissions()) { ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO), requestCode); return@setOnClickListener }
            val i = Intent(this, StreamService::class.java).putExtra("signalUrl", url.text.toString())
            ContextCompat.startForegroundService(this, i)
            start.isEnabled = false; stop.isEnabled = true; status.text = "Status: streaming — you can turn the screen off"
        }
        stop.setOnClickListener { stopService(Intent(this, StreamService::class.java)); start.isEnabled = true; stop.isEnabled = false; status.text = "Status: stopped" }
    }
    private fun hasPermissions() = ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED && ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
}
