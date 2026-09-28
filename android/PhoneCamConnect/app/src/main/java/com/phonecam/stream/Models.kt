package com.phonecam.stream

import kotlinx.coroutines.flow.MutableStateFlow

/**
 * Explicit application states (spec §9).
 * CAMERA_ACTIVE != STREAMING: STREAMING is only reported when WebRTC is connected
 * AND outbound video bytes are actually increasing.
 */
enum class StreamState {
    IDLE, STARTING, CAMERA_ACTIVE, DISCOVERABLE, CONNECTING,
    CONNECTED, STREAMING, DISCONNECTING, STOPPING, ERROR
}

enum class CameraFacing { BACK, FRONT;
    fun opposite() = if (this == BACK) FRONT else BACK
}

data class CameraMode(val width: Int, val height: Int, val fps: Int) {
    val label: String get() = "${width}x${height} @ ${fps} FPS"
}

data class UiState(
    val state: StreamState = StreamState.IDLE,
    val deviceId: String = "",
    val deviceName: String = "",
    val cameraActive: Boolean = false,
    val micEnabled: Boolean = true,
    val facing: CameraFacing = CameraFacing.BACK,
    val resolution: String = "",
    val signalingAddress: String? = null,
    val desktopAddress: String? = null,
    val error: String? = null
) {
    val isRunning: Boolean
        get() = state != StreamState.IDLE && state != StreamState.ERROR
}

/**
 * Process-wide bridge between StreamingService (owner) and the UI (observer).
 * The Activity only observes this and attaches/detaches a preview surface.
 */
object StreamingRepository {
    val ui = MutableStateFlow(UiState())
    val engine = MutableStateFlow<WebRtcEngine?>(null)
}