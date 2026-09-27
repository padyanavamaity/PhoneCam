package org.phonecam.connect.stream

import android.content.Context
import android.content.SharedPreferences

/** Camera lens facing (plan §6 config model). */
enum class CameraFacing { BACK, FRONT }

/** Video codec preference (RTP-only, applied at PeerConnection creation). */
enum class VideoCodec(val mimeType: String, val prefKey: String) {
    VP8("video/VP8", "vp8"),
    VP9("video/VP9", "vp9"),
    H264("video/H264", "h264");

    companion object {
        /** Unknown stored value -> VP8 (safe default, plan §6 validation). */
        fun fromKey(key: String?): VideoCodec = entries.firstOrNull { it.prefKey == key } ?: VP8
    }
}

/**
 * Immutable camera + stream configuration (plan §6).
 * Validation/clamping happens in [CameraConfigRepository.load], never here.
 */
data class CameraStreamConfig(
    val facing: CameraFacing = CameraFacing.BACK,
    val width: Int = 1280,
    val height: Int = 720,
    val fps: Int = 30,
    val videoBitrateBps: Int = 4_000_000,
    val codec: VideoCodec = VideoCodec.VP8
) {
    companion object {
        val DEFAULT = CameraStreamConfig()

        const val MIN_FPS = 15
        const val MAX_FPS = 60
        const val MIN_BITRATE_BPS = 500_000
        const val MAX_BITRATE_BPS = 16_000_000
    }
}

/**
 * Key-value persistence abstraction so the repository is JVM-testable
 * without Android (plan §6: `ConfigStore` + `InMemoryConfigStore`).
 */
interface ConfigStore {
    fun getString(key: String, def: String?): String?
    fun getInt(key: String, def: Int): Int
    fun putString(key: String, value: String)
    fun putInt(key: String, value: Int)
}

/** SharedPreferences-backed store — contains no secrets (Rule 4/12). */
class SharedPrefsConfigStore(context: Context) : ConfigStore {
    private val prefs: SharedPreferences =
        context.getSharedPreferences("camera_stream_config", Context.MODE_PRIVATE)

    override fun getString(key: String, def: String?): String? = prefs.getString(key, def)
    override fun getInt(key: String, def: Int): Int = prefs.getInt(key, def)
    override fun putString(key: String, value: String) {
        prefs.edit().putString(key, value).apply()
    }
    override fun putInt(key: String, value: Int) {
        prefs.edit().putInt(key, value).apply()
    }
}

/** Pure in-memory store for JVM unit tests. */
class InMemoryConfigStore : ConfigStore {
    private val strings = mutableMapOf<String, String>()
    private val ints = mutableMapOf<String, Int>()

    override fun getString(key: String, def: String?): String? = strings[key] ?: def
    override fun getInt(key: String, def: Int): Int = ints[key] ?: def
    override fun putString(key: String, value: String) {
        strings[key] = value
    }
    override fun putInt(key: String, value: Int) {
        ints[key] = value
    }
}

/** A supported capture size/fps bucket reported by Camera2Enumerator. */
data class SizeFps(val width: Int, val height: Int, val fps: Int)

/**
 * Load/validate/save for [CameraStreamConfig] (plan §6).
 *
 * Validation rules:
 *  - unknown enum value        -> default
 *  - bitrate clamped to 0.5..16 Mbps
 *  - fps clamped to 15..60
 *  - resolution snapped to nearest supported when a supported list is given
 */
class CameraConfigRepository(
    private val store: ConfigStore,
    private val log: (String) -> Unit = {}
) {
    companion object {
        const val KEY_FACING = "facing"
        const val KEY_WIDTH = "width"
        const val KEY_HEIGHT = "height"
        const val KEY_FPS = "fps"
        const val KEY_BITRATE = "video_bitrate_bps"
        const val KEY_CODEC = "video_codec"
    }

    fun load(supported: List<SizeFps>? = null): CameraStreamConfig {
        val def = CameraStreamConfig.DEFAULT

        val facing = when (store.getString(KEY_FACING, null)) {
            "FRONT" -> CameraFacing.FRONT
            "BACK", null -> CameraFacing.BACK
            else -> {
                log("Unknown facing value -> BACK")
                CameraFacing.BACK
            }
        }

        val codec = store.getString(KEY_CODEC, null).let { raw ->
            val parsed = VideoCodec.fromKey(raw)
            if (raw != null && parsed.prefKey != raw) log("Unknown codec '$raw' -> VP8")
            parsed
        }

        val fps = clampInt(store.getInt(KEY_FPS, def.fps), CameraStreamConfig.MIN_FPS, CameraStreamConfig.MAX_FPS, "fps")
        val bitrate = clampInt(store.getInt(KEY_BITRATE, def.videoBitrateBps), CameraStreamConfig.MIN_BITRATE_BPS, CameraStreamConfig.MAX_BITRATE_BPS, "bitrate")

        var width = store.getInt(KEY_WIDTH, def.width)
        var height = store.getInt(KEY_HEIGHT, def.height)
        if (supported != null && supported.isNotEmpty()) {
            val snapped = nearestSupported(width, height, supported)
            if (snapped != null && (snapped.width != width || snapped.height != height)) {
                log("Resolution ${width}x${height} not supported -> ${snapped.width}x${snapped.height}")
                width = snapped.width
                height = snapped.height
            }
        }

        return CameraStreamConfig(
            facing = facing,
            width = width,
            height = height,
            fps = fps,
            videoBitrateBps = bitrate,
            codec = codec
        )
    }

    fun save(config: CameraStreamConfig) {
        store.putString(KEY_FACING, config.facing.name)
        store.putInt(KEY_WIDTH, config.width)
        store.putInt(KEY_HEIGHT, config.height)
        store.putInt(KEY_FPS, config.fps)
        store.putInt(KEY_BITRATE, config.videoBitrateBps)
        store.putString(KEY_CODEC, config.codec.prefKey)
    }

    private fun clampInt(value: Int, min: Int, max: Int, name: String): Int {
        val clamped = value.coerceIn(min, max)
        if (clamped != value) log("$name $value clamped to $clamped")
        return clamped
    }

    private fun nearestSupported(width: Int, height: Int, supported: List<SizeFps>): SizeFps? {
        if (supported.any { it.width == width && it.height == height }) {
            return SizeFps(width, height, 0)
        }
        // Nearest by pixel-count, tie-break by larger height.
        return supported.minByOrNull {
            val d = it.width.toLong() * it.height - width.toLong() * height
            kotlin.math.abs(d)
        }
    }
}
