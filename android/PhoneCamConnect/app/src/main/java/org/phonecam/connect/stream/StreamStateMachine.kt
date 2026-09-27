package org.phonecam.connect.stream

/**
 * Streaming pipeline phases (plan §3.1).
 *
 * IDLE -> STARTING_CAMERA -> CAMERA_ACTIVE is the reachable end-state of this
 * milestone; WEBRTC_READY and beyond are wired + unit-tested but unreachable
 * at runtime until the signaling task lands.
 */
enum class StreamPhase {
    IDLE,
    STARTING_CAMERA,
    CAMERA_ACTIVE,
    WEBRTC_READY,
    CONNECTING,
    CONNECTED,
    STREAMING,
    STOPPING,
    ERROR
}

/**
 * Events that drive the stream state machine (plan §3.2).
 */
sealed interface StreamEvent {
    data object StartRequested : StreamEvent
    data object StopRequested : StreamEvent
    data object CameraOpened : StreamEvent
    data class CameraFailed(val message: String) : StreamEvent
    data object PeerConnectionReady : StreamEvent
    data object SignalingStarted : StreamEvent
    data object IceConnected : StreamEvent
    data object IceDisconnected : StreamEvent
    data object IceFailed : StreamEvent
    data object MediaFlowing : StreamEvent
    data object TeardownComplete : StreamEvent
    data object RetryRequested : StreamEvent
    data object ErrorCleared : StreamEvent
}

/**
 * Transition table per plan §3.3. Pure Kotlin, zero Android dependencies —
 * fully JVM-unit-testable. Rejected events return false and are logged via
 * [onRejected]; the machine never throws on unexpected input.
 */
class StreamStateMachine(
    initial: StreamPhase = StreamPhase.IDLE,
    private val onTransition: (from: StreamPhase, to: StreamPhase, event: StreamEvent) -> Unit = { _, _, _ -> },
    private val onRejected: (phase: StreamPhase, event: StreamEvent) -> Unit = { _, _ -> }
) {
    private data class PhaseEvent(val from: StreamPhase, val event: StreamEvent)

    private val transitions: Map<PhaseEvent, StreamPhase>

    init {
        val t = mutableMapOf<PhaseEvent, StreamPhase>()

        fun add(from: StreamPhase, event: StreamEvent, to: StreamPhase) {
            t[PhaseEvent(from, event)] = to
        }

        // Reachable now (pre-signaling)
        add(StreamPhase.IDLE, StreamEvent.StartRequested, StreamPhase.STARTING_CAMERA)
        add(StreamPhase.STARTING_CAMERA, StreamEvent.CameraOpened, StreamPhase.CAMERA_ACTIVE)
        add(StreamPhase.STARTING_CAMERA, StreamEvent.StopRequested, StreamPhase.STOPPING)
        add(StreamPhase.CAMERA_ACTIVE, StreamEvent.StopRequested, StreamPhase.STOPPING)
        add(StreamPhase.STOPPING, StreamEvent.TeardownComplete, StreamPhase.IDLE)
        add(StreamPhase.ERROR, StreamEvent.RetryRequested, StreamPhase.STARTING_CAMERA)
        add(StreamPhase.ERROR, StreamEvent.StopRequested, StreamPhase.STOPPING)
        add(StreamPhase.ERROR, StreamEvent.ErrorCleared, StreamPhase.IDLE)

        // CameraFailed is legal from ANY state except terminal ones.
        for (phase in StreamPhase.entries) {
            if (phase != StreamPhase.ERROR && phase != StreamPhase.STOPPING) {
                add(phase, StreamEvent.CameraFailed(""), StreamPhase.ERROR)
            }
        }

        // Future (post-signaling) — locked in by unit tests so the signaling
        // task cannot regress them.
        add(StreamPhase.CAMERA_ACTIVE, StreamEvent.PeerConnectionReady, StreamPhase.WEBRTC_READY)
        add(StreamPhase.WEBRTC_READY, StreamEvent.SignalingStarted, StreamPhase.CONNECTING)
        add(StreamPhase.WEBRTC_READY, StreamEvent.StopRequested, StreamPhase.STOPPING)
        add(StreamPhase.CONNECTING, StreamEvent.IceConnected, StreamPhase.CONNECTED)
        add(StreamPhase.CONNECTING, StreamEvent.IceFailed, StreamPhase.ERROR)
        add(StreamPhase.CONNECTING, StreamEvent.StopRequested, StreamPhase.STOPPING)
        add(StreamPhase.CONNECTED, StreamEvent.MediaFlowing, StreamPhase.STREAMING)
        add(StreamPhase.CONNECTED, StreamEvent.IceDisconnected, StreamPhase.CONNECTING)
        add(StreamPhase.CONNECTED, StreamEvent.StopRequested, StreamPhase.STOPPING)
        add(StreamPhase.STREAMING, StreamEvent.IceDisconnected, StreamPhase.CONNECTING)
        add(StreamPhase.STREAMING, StreamEvent.StopRequested, StreamPhase.STOPPING)

        transitions = t
    }

    @get:Synchronized
    var phase: StreamPhase = initial
        private set

    /**
     * Submit an event. Returns true if the transition was legal and applied,
     * false if rejected (logged via [onRejected], never throws).
     */
    @Synchronized
    fun submit(event: StreamEvent): Boolean {
        // CameraFailed carries a message — match on type, not instance equality.
        val target = if (event is StreamEvent.CameraFailed) {
            transitions[PhaseEvent(phase, StreamEvent.CameraFailed(""))]
        } else {
            transitions[PhaseEvent(phase, event)]
        }

        if (target == null) {
            onRejected(phase, event)
            return false
        }
        val from = phase
        phase = target
        onTransition(from, target, event)
        return true
    }

    @Synchronized
    fun reset(to: StreamPhase = StreamPhase.IDLE) {
        phase = to
    }
}

/**
 * Derived UI-facing state bundle (plan §3.4).
 *
 * [micActive] is derived, honest: microphone hardware is only opened by the
 * ADM when an active PeerConnection pulls the audio source, which requires
 * signaling. Until then it ALWAYS reports false — privacy-correct.
 */
data class ServiceState(
    val phase: StreamPhase = StreamPhase.IDLE,
    val errorMessage: String? = null,
    val cameraFacing: CameraFacing = CameraFacing.BACK,
    val isStreaming: Boolean = phase == StreamPhase.STREAMING,
    val micActive: Boolean = phase == StreamPhase.CONNECTED || phase == StreamPhase.STREAMING,
    val cameraActive: Boolean = phase == StreamPhase.CAMERA_ACTIVE ||
            phase == StreamPhase.WEBRTC_READY ||
            phase == StreamPhase.CONNECTING ||
            phase == StreamPhase.CONNECTED ||
            phase == StreamPhase.STREAMING,
    val measuredWidth: Int = 0,
    val measuredHeight: Int = 0,
    val measuredFps: Int = 0
)
