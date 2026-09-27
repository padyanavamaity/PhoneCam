package org.phonecam.connect.stream

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class StreamStateMachineTest {

    private fun m(): StreamStateMachine = StreamStateMachine()

    // ---- Happy path: IDLE -> STARTING_CAMERA -> CAMERA_ACTIVE ----

    @Test
    fun `idle startRequested goes to startingCamera`() {
        val m = m()
        assertTrue(m.submit(StreamEvent.StartRequested))
        assertEquals(StreamPhase.STARTING_CAMERA, m.phase)
    }

    @Test
    fun `startingCamera cameraOpened goes to cameraActive`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        assertTrue(m.submit(StreamEvent.CameraOpened))
        assertEquals(StreamPhase.CAMERA_ACTIVE, m.phase)
    }

    @Test
    fun `startingCamera cameraFailed goes to error`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        assertTrue(m.submit(StreamEvent.CameraFailed("boom")))
        assertEquals(StreamPhase.ERROR, m.phase)
    }

    // ---- Rejections / duplicate-start guard ----

    @Test
    fun `idle cameraOpened rejected`() {
        val m = m()
        assertFalse(m.submit(StreamEvent.CameraOpened))
        assertEquals(StreamPhase.IDLE, m.phase)
    }

    @Test
    fun `idle stopRequested rejected`() {
        val m = m()
        assertFalse(m.submit(StreamEvent.StopRequested))
        assertEquals(StreamPhase.IDLE, m.phase)
    }

    @Test
    fun `startingCamera startRequested rejected (duplicate start)`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        assertFalse(m.submit(StreamEvent.StartRequested))
        assertEquals(StreamPhase.STARTING_CAMERA, m.phase)
    }

    @Test
    fun `cameraActive startRequested rejected`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraOpened)
        assertFalse(m.submit(StreamEvent.StartRequested))
        assertEquals(StreamPhase.CAMERA_ACTIVE, m.phase)
    }

    @Test
    fun `stopping stopRequested rejected (duplicate stop)`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraOpened)
        m.submit(StreamEvent.StopRequested)
        assertEquals(StreamPhase.STOPPING, m.phase)
        assertFalse(m.submit(StreamEvent.StopRequested))
        assertEquals(StreamPhase.STOPPING, m.phase)
    }

    // ---- Stop / teardown ----

    @Test
    fun `cameraActive stopRequested goes to stopping then idle`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraOpened)
        assertTrue(m.submit(StreamEvent.StopRequested))
        assertEquals(StreamPhase.STOPPING, m.phase)
        assertTrue(m.submit(StreamEvent.TeardownComplete))
        assertEquals(StreamPhase.IDLE, m.phase)
    }

    @Test
    fun `startingCamera stopRequested goes to stopping`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        assertTrue(m.submit(StreamEvent.StopRequested))
        assertEquals(StreamPhase.STOPPING, m.phase)
    }

    @Test
    fun `full cycle start stop start stop`() {
        val m = m()
        // First cycle
        assertTrue(m.submit(StreamEvent.StartRequested))
        assertTrue(m.submit(StreamEvent.CameraOpened))
        assertTrue(m.submit(StreamEvent.StopRequested))
        assertTrue(m.submit(StreamEvent.TeardownComplete))
        assertEquals(StreamPhase.IDLE, m.phase)
        // Second cycle — must behave identically
        assertTrue(m.submit(StreamEvent.StartRequested))
        assertEquals(StreamPhase.STARTING_CAMERA, m.phase)
        assertTrue(m.submit(StreamEvent.CameraOpened))
        assertEquals(StreamPhase.CAMERA_ACTIVE, m.phase)
        assertTrue(m.submit(StreamEvent.StopRequested))
        assertEquals(StreamPhase.STOPPING, m.phase)
        assertTrue(m.submit(StreamEvent.TeardownComplete))
        assertEquals(StreamPhase.IDLE, m.phase)
    }

    // ---- Error recovery ----

    @Test
    fun `error retryRequested goes to startingCamera`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraFailed("x"))
        assertTrue(m.submit(StreamEvent.RetryRequested))
        assertEquals(StreamPhase.STARTING_CAMERA, m.phase)
    }

    @Test
    fun `error errorCleared goes to idle`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraFailed("x"))
        assertTrue(m.submit(StreamEvent.ErrorCleared))
        assertEquals(StreamPhase.IDLE, m.phase)
    }

    @Test
    fun `error stopRequested goes to stopping`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraFailed("x"))
        assertTrue(m.submit(StreamEvent.StopRequested))
        assertEquals(StreamPhase.STOPPING, m.phase)
    }

    // ---- CameraFailed from any non-terminal state ----

    @Test
    fun `cameraFailed from cameraActive goes to error`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraOpened)
        assertTrue(m.submit(StreamEvent.CameraFailed("disconnected")))
        assertEquals(StreamPhase.ERROR, m.phase)
    }

    @Test
    fun `cameraFailed from error rejected (already error)`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraFailed("a"))
        assertFalse(m.submit(StreamEvent.CameraFailed("b")))
        assertEquals(StreamPhase.ERROR, m.phase)
    }

    // ---- Future (post-signaling) paths ----

    @Test
    fun `cameraActive peerConnectionReady goes to webrtcReady`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraOpened)
        assertTrue(m.submit(StreamEvent.PeerConnectionReady))
        assertEquals(StreamPhase.WEBRTC_READY, m.phase)
    }

    @Test
    fun `webrtcReady signalingStarted goes to connecting`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraOpened)
        m.submit(StreamEvent.PeerConnectionReady)
        assertTrue(m.submit(StreamEvent.SignalingStarted))
        assertEquals(StreamPhase.CONNECTING, m.phase)
    }

    @Test
    fun `connecting iceConnected goes to connected`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraOpened)
        m.submit(StreamEvent.PeerConnectionReady)
        m.submit(StreamEvent.SignalingStarted)
        assertTrue(m.submit(StreamEvent.IceConnected))
        assertEquals(StreamPhase.CONNECTED, m.phase)
    }

    @Test
    fun `connected mediaFlowing goes to streaming`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraOpened)
        m.submit(StreamEvent.PeerConnectionReady)
        m.submit(StreamEvent.SignalingStarted)
        m.submit(StreamEvent.IceConnected)
        assertTrue(m.submit(StreamEvent.MediaFlowing))
        assertEquals(StreamPhase.STREAMING, m.phase)
    }

    @Test
    fun `streaming iceDisconnected back to connecting`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraOpened)
        m.submit(StreamEvent.PeerConnectionReady)
        m.submit(StreamEvent.SignalingStarted)
        m.submit(StreamEvent.IceConnected)
        m.submit(StreamEvent.MediaFlowing)
        assertTrue(m.submit(StreamEvent.IceDisconnected))
        assertEquals(StreamPhase.CONNECTING, m.phase)
    }

    @Test
    fun `connecting iceFailed goes to error`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraOpened)
        m.submit(StreamEvent.PeerConnectionReady)
        m.submit(StreamEvent.SignalingStarted)
        assertTrue(m.submit(StreamEvent.IceFailed))
        assertEquals(StreamPhase.ERROR, m.phase)
    }

    @Test
    fun `onTransition callback invoked on accepted events`() {
        val calls = mutableListOf<Triple<StreamPhase, StreamPhase, StreamEvent>>()
        val m = StreamStateMachine(
            initial = StreamPhase.IDLE,
            onTransition = { f, t, e -> calls.add(Triple(f, t, e)) },
            onRejected = { _, _ -> }
        )
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraOpened)
        assertEquals(2, calls.size)
        assertEquals(Triple(StreamPhase.IDLE, StreamPhase.STARTING_CAMERA, StreamEvent.StartRequested), calls[0])
        assertEquals(Triple(StreamPhase.STARTING_CAMERA, StreamPhase.CAMERA_ACTIVE, StreamEvent.CameraOpened), calls[1])
    }

    @Test
    fun `onRejected callback invoked on rejected events`() {
        val rejected = mutableListOf<StreamEvent>()
        val m = StreamStateMachine(
            onTransition = { _, _, _ -> },
            onRejected = { _, e -> rejected.add(e) }
        )
        assertFalse(m.submit(StreamEvent.CameraOpened))
        assertEquals(1, rejected.size)
    }

    // ---- Derived ServiceState rules ----

    @Test
    fun `micActive is false until connected`() {
        for (phase in StreamPhase.entries) {
            val s = ServiceState(phase = phase)
            val expected = phase == StreamPhase.CONNECTED || phase == StreamPhase.STREAMING
            assertEquals("phase=$phase", expected, s.micActive)
        }
    }

    @Test
    fun `cameraActive true only when capture pipeline up`() {
        for (phase in StreamPhase.entries) {
            val s = ServiceState(phase = phase)
            val expected = phase == StreamPhase.CAMERA_ACTIVE ||
                    phase == StreamPhase.WEBRTC_READY ||
                    phase == StreamPhase.CONNECTING ||
                    phase == StreamPhase.CONNECTED ||
                    phase == StreamPhase.STREAMING
            assertEquals("phase=$phase", expected, s.cameraActive)
        }
    }

    // ---- UI mapping: mirror of MainActivity.updateUi rules ----
    //
    // These tests pin the *contract* the UI implements: which phases map to
    // Start / Starting / Stop / Stopping, and which mean "Camera Active
    // (Locally)" vs "Camera Active (Streaming)". If the machine transitions
    // change, these tests must be revisited alongside updateUi().

    private enum class UiBtn { START, STARTING, STOP, STOPPING }

    private fun buttonFor(phase: StreamPhase): UiBtn = when (phase) {
        StreamPhase.IDLE, StreamPhase.ERROR -> UiBtn.START
        StreamPhase.STARTING_CAMERA -> UiBtn.STARTING
        StreamPhase.STOPPING -> UiBtn.STOPPING
        StreamPhase.CAMERA_ACTIVE, StreamPhase.WEBRTC_READY,
        StreamPhase.CONNECTING, StreamPhase.CONNECTED,
        StreamPhase.STREAMING -> UiBtn.STOP
    }

    private fun buttonEnabled(phase: StreamPhase): Boolean = when (phase) {
        StreamPhase.IDLE, StreamPhase.ERROR,
        StreamPhase.CAMERA_ACTIVE, StreamPhase.WEBRTC_READY,
        StreamPhase.CONNECTING, StreamPhase.CONNECTED, StreamPhase.STREAMING -> true
        StreamPhase.STARTING_CAMERA, StreamPhase.STOPPING -> false
    }

    @Test
    fun `ui mapping startingCamera shows Starting not Stopping`() {
        assertEquals(UiBtn.STARTING, buttonFor(StreamPhase.STARTING_CAMERA))
    }

    @Test
    fun `ui mapping stopping shows Stopping`() {
        assertEquals(UiBtn.STOPPING, buttonFor(StreamPhase.STOPPING))
    }

    @Test
    fun `ui mapping cameraActive shows Stop`() {
        assertEquals(UiBtn.STOP, buttonFor(StreamPhase.CAMERA_ACTIVE))
    }

    @Test
    fun `ui mapping all active phases show Stop`() {
        assertEquals(UiBtn.STOP, buttonFor(StreamPhase.CONNECTED))
        assertEquals(UiBtn.STOP, buttonFor(StreamPhase.STREAMING))
    }

    @Test
    fun `ui mapping button disabled during transitions`() {
        assertFalse(buttonEnabled(StreamPhase.STARTING_CAMERA))
        assertFalse(buttonEnabled(StreamPhase.STOPPING))
        assertTrue(buttonEnabled(StreamPhase.IDLE))
        assertTrue(buttonEnabled(StreamPhase.CAMERA_ACTIVE))
    }

    @Test
    fun `ui mapping local vs streaming distinction`() {
        // Locally active = camera active but no peer-connected media yet.
        val localPhases = setOf(StreamPhase.CAMERA_ACTIVE, StreamPhase.WEBRTC_READY)
        // Camera pipeline active (video flowing) — display "Camera Active (…)".
        val activePhases = localPhases + setOf(
            StreamPhase.CONNECTING, StreamPhase.CONNECTED, StreamPhase.STREAMING
        )
        for (phase in StreamPhase.entries) {
            val s = ServiceState(phase = phase)
            when {
                phase == StreamPhase.STREAMING -> {
                    assertTrue("STREAMING active", s.cameraActive)
                    assertTrue("STREAMING streaming", s.isStreaming)
                }
                phase in localPhases -> {
                    assertTrue("phase=$phase locally active", s.cameraActive)
                    assertFalse("phase=$phase not streaming", s.isStreaming)
                }
                phase == StreamPhase.CONNECTED -> {
                    // WebRTC connected; media not yet flagged as flowing.
                    assertTrue("phase=$phase cameraActive", s.cameraActive)
                    assertFalse("phase=$phase not streaming yet", s.isStreaming)
                }
                phase in activePhases -> assertTrue("phase=$phase cameraActive", s.cameraActive)
                else -> {
                    assertFalse("phase=$phase camera inactive", s.cameraActive)
                    assertFalse("phase=$phase not streaming", s.isStreaming)
                }
            }
        }
    }

    // ---- Late-binding contract ----
    //
    // A newly attached Activity receives the *current* state immediately
    // because serviceState is a StateFlow (conflated, replays latest value).
    // Pin that the machine phase is directly observable at any time.

    @Test
    fun `current phase observable mid-transition for late subscriber`() {
        val m = m()
        m.submit(StreamEvent.StartRequested)
        m.submit(StreamEvent.CameraOpened)
        // Late subscriber reads phase synchronously — no event replay needed.
        assertEquals(StreamPhase.CAMERA_ACTIVE, m.phase)
    }
}
