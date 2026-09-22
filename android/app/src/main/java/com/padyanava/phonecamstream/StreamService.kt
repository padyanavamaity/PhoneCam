package com.padyanava.phonecamstream

import android.app.*
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.*
import androidx.core.app.NotificationCompat
import org.webrtc.*
import okhttp3.*
import org.json.JSONObject

class StreamService : Service() {
    private lateinit var wake: PowerManager.WakeLock
    private var ws: WebSocket? = null
    private var peer: PeerConnection? = null
    private var factory: PeerConnectionFactory? = null
    private var capturer: VideoCapturer? = null
    private val egl = EglBase.create()

    override fun onCreate() {
        super.onCreate()
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "PhoneCamStream:Stream").apply { setReferenceCounted(false); acquire() }
        createChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val url = intent?.getStringExtra("signalUrl") ?: return START_NOT_STICKY
        val notification = NotificationCompat.Builder(this, "stream")
            .setContentTitle("PhoneCam Stream is live").setContentText("Camera streaming to PC • screen may be off")
            .setSmallIcon(android.R.drawable.presence_video_online).setOngoing(true).build()
        startForeground(42, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
        startWebRtc(url)
        return START_NOT_STICKY
    }

    private fun startWebRtc(url: String) {
        PeerConnectionFactory.initialize(PeerConnectionFactory.InitializationOptions.builder(this).createInitializationOptions())
        factory = PeerConnectionFactory.builder().setVideoEncoderFactory(DefaultVideoEncoderFactory(egl.eglBaseContext, true, true)).setVideoDecoderFactory(DefaultVideoDecoderFactory(egl.eglBaseContext)).createPeerConnectionFactory()
        val ice = listOf(PeerConnection.IceServer.builder("stun:stun.l.google.com:19302").createIceServer())
        val rtc = PeerConnection.RTCConfiguration(ice)
        peer = factory!!.createPeerConnection(rtc, object: PeerConnection.Observer {
            override fun onIceCandidate(c: IceCandidate) { send(JSONObject().put("type","candidate").put("sdpMid",c.sdpMid).put("sdpMLineIndex",c.sdpMLineIndex).put("candidate",c.sdp)) }
            override fun onSignalingChange(p: PeerConnection.SignalingState) {}
            override fun onIceConnectionChange(s: PeerConnection.IceConnectionState) {}
            override fun onIceConnectionReceivingChange(b: Boolean) {}
            override fun onIceGatheringChange(s: PeerConnection.IceGatheringState) {}
            override fun onAddStream(s: MediaStream) {}
            override fun onRemoveStream(s: MediaStream) {}
            override fun onDataChannel(d: DataChannel) {}
            override fun onRenegotiationNeeded() {}
            override fun onAddTrack(r: RtpReceiver, m: Array<out MediaStream>) {}
            override fun onConnectionChange(n: PeerConnection.PeerConnectionState) {}
            override fun onSelectedCandidatePairChanged(e: CandidatePairChangeEvent) {}
        })
        capturer = createCapturer()
        val source = factory!!.createVideoSource(false)
        capturer!!.initialize(SurfaceTextureHelper.create("CameraThread", egl.eglBaseContext), this, source.capturerObserver)
        capturer!!.startCapture(1920, 1080, 30)
        val track = factory!!.createVideoTrack("video0", source)
        peer!!.addTrack(track, listOf("stream0"))
        connect(url)
    }

    private fun createCapturer(): VideoCapturer {
        val e = Camera2Enumerator(this)
        val name = e.deviceNames.firstOrNull { e.isBackFacing(it) } ?: e.deviceNames.first()
        return e.createCapturer(name, null)
    }

    private fun connect(url: String) {
        val client = OkHttpClient()
        ws = client.newWebSocket(Request.Builder().url(url).build(), object: WebSocketListener() {
            override fun onOpen(w: WebSocket, r: Response) { w.send(JSONObject().put("role","phone").toString()) }
            override fun onMessage(w: WebSocket, text: String) {
                val j = JSONObject(text)
                when(j.optString("type")) {
                    "viewer-ready" -> createOffer()
                    "answer" -> peer?.setRemoteDescription(SimpleSdpObserver(), SessionDescription(SessionDescription.Type.ANSWER, j.getString("sdp")))
                    "candidate" -> peer?.addIceCandidate(IceCandidate(j.optString("sdpMid"), j.getInt("sdpMLineIndex"), j.getString("candidate")))
                }
            }
        })
    }
    private fun createOffer() { peer?.createOffer(object: SimpleSdpObserver(){ override fun onCreateSuccess(d: SessionDescription){ peer?.setLocalDescription(SimpleSdpObserver(), d); send(JSONObject().put("type","offer").put("sdp",d.description)) } }, MediaConstraints()) }
    private fun send(j: JSONObject) { ws?.send(j.toString()) }
    private fun createChannel() { (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(NotificationChannel("stream","Streaming",NotificationManager.IMPORTANCE_LOW)) }
    override fun onDestroy() { try { capturer?.stopCapture() } catch(_: Exception){}; capturer?.dispose(); peer?.dispose(); factory?.dispose(); ws?.close(1000,null); if(wake.isHeld) wake.release(); egl.release(); super.onDestroy() }
    override fun onBind(intent: Intent?) : IBinder? = null
}

open class SimpleSdpObserver : SdpObserver {
    override fun onCreateSuccess(s: SessionDescription) {}
    override fun onSetSuccess() {}
    override fun onCreateFailure(s: String) {}
    override fun onSetFailure(s: String) {}
}
