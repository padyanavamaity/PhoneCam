package com.phonecam.stream

import android.content.Context
import java.util.UUID

/** Persistent device identity (spec §10) + user settings (spec §28). */
class AppSettings(context: Context) {
    private val p = context.applicationContext
        .getSharedPreferences("phonecam_settings", Context.MODE_PRIVATE)

    /** Stable across restarts, e.g. "PhoneCam-7A31". */
    val deviceId: String
        get() = p.getString("device_id", null) ?: generateId().also {
            p.edit().putString("device_id", it).apply()
        }

    /** User-visible name ("Front Camera"); defaults to the device id. */
    var deviceName: String
        get() = p.getString("device_name", null)?.takeIf { it.isNotBlank() } ?: deviceId
        set(v) = p.edit().putString("device_name", v.trim()).apply()

    var facing: CameraFacing
        get() = runCatching { CameraFacing.valueOf(p.getString("facing", "BACK")!!) }
            .getOrDefault(CameraFacing.BACK)
        set(v) = p.edit().putString("facing", v.name).apply()

    var micEnabled: Boolean
        get() = p.getBoolean("mic", true)
        set(v) = p.edit().putBoolean("mic", v).apply()

    var width: Int
        get() = p.getInt("width", 1280)
        set(v) = p.edit().putInt("width", v).apply()

    var height: Int
        get() = p.getInt("height", 720)
        set(v) = p.edit().putInt("height", v).apply()

    var fps: Int
        get() = p.getInt("fps", 30)
        set(v) = p.edit().putInt("fps", v).apply()

    var bitrateKbps: Int
        get() = p.getInt("bitrate_kbps", 4000)
        set(v) = p.edit().putInt("bitrate_kbps", v.coerceIn(300, 25000)).apply()

    var autoStart: Boolean
        get() = p.getBoolean("auto_start", false)
        set(v) = p.edit().putBoolean("auto_start", v).apply()

    private fun generateId(): String =
        "PhoneCam-" + UUID.randomUUID().toString().replace("-", "").take(4).uppercase()
}