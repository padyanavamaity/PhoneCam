package org.phonecam.connect.stream

import android.content.Context
import android.media.MediaRecorder
import android.util.Log
import org.webrtc.AudioSource
import org.webrtc.AudioTrack
import org.webrtc.MediaConstraints
import org.webrtc.PeerConnectionFactory
import org.webrtc.audio.JavaAudioDeviceModule

/**
 * Explicit audio pipeline (plan §5.5).
 *
 * Uses [JavaAudioDeviceModule] with VOICE_COMMUNICATION source so Android
 * routes microphone through the platform AEC/NS path when available.
 *
 * IMPORTANT privacy rule (plan §3.4 / Android Privacy): the ADM only opens
 * microphone hardware when an active PeerConnection pulls the [AudioSource].
 * Creating the track here does NOT touch the mic — that begins only when the
 * future signaling task adds this track to a PeerConnection and it negotiates.
 * Until then, `micActive` in the UI must report false.
 */
class AudioEngine(
    private val context: Context
) {
    private val TAG = "AudioEngine"

    var audioDeviceModule: JavaAudioDeviceModule? = null
        private set
    var audioSource: AudioSource? = null
        private set
    var audioTrack: AudioTrack? = null
        private set

    /** True once ADM exists and is wired into the factory (not the same as mic open). */
    val isInitialized: Boolean
        get() = audioDeviceModule != null

    /**
     * Build the ADM. Must be called BEFORE the PeerConnectionFactory is created,
     * since the ADM is passed into the factory builder. Safe to call once;
     * subsequent calls are ignored (idempotent).
     */
    @Synchronized
    fun initialize(factoryBuilder: PeerConnectionFactory.Builder) {
        if (audioDeviceModule != null) {
            Log.d(TAG, "initialize() ignored — already initialized")
            return
        }
        val adm = JavaAudioDeviceModule.builder(context)
            .setAudioSource(MediaRecorder.AudioSource.VOICE_COMMUNICATION) // platform AEC/NS routing
            .setUseHardwareAcousticEchoCanceler(true)
            .setUseHardwareNoiseSuppressor(true)
            .setUseStereoInput(false)   // mono -> Opus
            .setUseStereoOutput(false)
            .createAudioDeviceModule()
        audioDeviceModule = adm
        factoryBuilder.setAudioDeviceModule(adm)
        Log.d(TAG, "JavaAudioDeviceModule created (VOICE_COMMUNICATION, hw AEC/NS)")
    }

    /**
     * Create the AudioSource/AudioTrack after the factory exists. Track is
     * created ENABLED but is not consumed until a PeerConnection exists —
     * mic hardware remains closed until then.
     */
    @Synchronized
    fun createTrack(factory: PeerConnectionFactory) {
        check(audioDeviceModule != null) { "initialize() must run before createTrack()" }
        if (audioTrack != null) {
            Log.d(TAG, "createTrack() ignored — track exists")
            return
        }
        val source = factory.createAudioSource(MediaConstraints())
        audioSource = source
        audioTrack = factory.createAudioTrack(TRACK_ID, source)
        Log.d(TAG, "AudioSource + AudioTrack created (id=$TRACK_ID, not yet capturing)")
    }

    /**
     * Mute/unmute the outbound track. This is the version-independent mute:
     * encoding stops so no real audio is sent. It does NOT open the mic
     * when unmuting if no PeerConnection is consuming the track.
     */
    fun setMuted(muted: Boolean) {
        audioTrack?.setEnabled(!muted)
        Log.d(TAG, "setMuted($muted)")
    }

    /** Fixed disposal order. Safe to call multiple times. */
    @Synchronized
    fun release() {
        runCatching { audioTrack?.dispose() }
        runCatching { audioSource?.dispose() }
        audioDeviceModule?.let { adm: JavaAudioDeviceModule -> runCatching { adm.release() } }
        audioTrack = null
        audioSource = null
        audioDeviceModule = null
        Log.d(TAG, "AudioEngine released")
    }

    companion object {
        const val TRACK_ID = "ARDAMSa0"
    }
}
