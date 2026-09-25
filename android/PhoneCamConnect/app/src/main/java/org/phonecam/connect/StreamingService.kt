package org.phonecam.connect

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.SurfaceTexture
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.IBinder
import android.os.Looper
import android.util.Log
import android.util.Size
import android.view.Surface
import androidx.camera.core.CameraSelector
import androidx.camera.core.Preview
import androidx.camera.core.VideoCapture
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.video.Recorder
import androidx.camera.video.Recording
import androidx.camera.video.VideoRecordEvent
import androidx.camera.video.output.FileOutputOptions
import androidx.core.content.ContextCompat
import com.google.common.util.concurrent.ListenableFuture
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import org.webrtc.AudioSource
import org.webrtc.AudioTrack
import org.webrtc.DefaultVideoDecoderFactory
import org.webrtc.DefaultVideoEncoderFactory
import org.webrtc.EglBase
import org.webrtc.IceCandidate
import org.webrtc.JavaAudioDeviceModule
import org.webrtc.MediaConstraints
import org.webrtc.MediaStream
import org.webrtc.PeerConnection
import org.webrtc.PeerConnectionFactory
import org.webrtc.RtpTransceiver
import org.webrtc.SessionDescription
import org.webrtc.SurfaceTextureHelper
import org.webrtc.VideoFrame
import org.webrtc.VideoSink
import org.webrtc.VideoSource
import org.webrtc.VideoTrack
import org.webrtc.audio.AudioCapturer
import java.nio.ByteBuffer
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

class StreamingService : Service() {

    private val TAG = "StreamingService"
    
    // Configuration
    private val VIDEO_WIDTH = 1280
    private val VIDEO_HEIGHT = 720
    private val VIDEO_FPS = 30
    private val AUDIO_SAMPLE_RATE = 48000
    private val AUDIO_CHANNELS = 2
    
    // WebRTC
    private var peerConnectionFactory: PeerConnectionFactory? = null
    private var peerConnection: PeerConnection? = null
    private var videoSource: VideoSource? = null
    private var videoTrack: VideoTrack? = null
    private var audioSource: AudioSource? = null
    private var audioTrack: AudioTrack? = null
    private var localMediaStream: MediaStream? = null
    private var eglBase: EglBase? = null
    private var surfaceTextureHelper: SurfaceTextureHelper? = null
    private var cameraSurfaceTexture: SurfaceTexture? = null
    private var cameraSurface: Surface? = null
    
    // CameraX
    private var cameraExecutor: ExecutorService? = null
    private var preview: Preview? = null
    private var videoCapture: VideoCapture? = null
    
    // Audio Capturer (WebRTC native)
    private var audioCapturer: AndroidAudioCapturer? = null
    private var javaAudioDeviceModule: JavaAudioDeviceModule? = null
    
    // Coroutines
    private var coroutineScope: CoroutineScope? = null
    private var mainJob: Job? = null
    
    // Signaling (placeholder - would connect to actual signaling server)
    private var signalingConnected = false
    
    override fun onCreate() {
        super.onCreate()
        
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel("phonecam_stream", "PhoneCam streaming", NotificationManager.IMPORTANCE_LOW)
        )
        
        // Initialize WebRTC
        initializeWebRTC()
        
        // Initialize coroutines
        mainJob = Job()
        coroutineScope = CoroutineScope(Dispatchers.Default + mainJob!!)
        
        // Initialize executors
        cameraExecutor = Executors.newSingleThreadExecutor()
    }
    
    private fun initializeWebRTC() {
        // Initialize EGL base for video rendering
        eglBase = EglBase.create()
        
        // Initialize PeerConnectionFactory with JavaAudioDeviceModule for audio capture
        val initializerOptions = PeerConnectionFactory.InitializationOptions.builder(this)
            .setEnableInternalTracer(true)
            .createInitializationOptions()
        PeerConnectionFactory.initialize(initializerOptions)
        
        val options = PeerConnectionFactory.Options()
        options.networkIgnoreMask = 0
        options.disableEncryption = false
        options.disableNetworkMonitor = false
        
        // Create JavaAudioDeviceModule with custom audio capturer
        javaAudioDeviceModule = JavaAudioDeviceModule.builder(this)
            .setUseHardwareAcousticEchoCanceler(false)
            .setUseHardwareNoiseSuppressor(false)
            .createAudioDeviceModule()
        
        peerConnectionFactory = PeerConnectionFactory.builder()
            .setOptions(options)
            .setVideoEncoderFactory(DefaultVideoEncoderFactory(eglBase!!.eglBaseContext, true, true))
            .setVideoDecoderFactory(DefaultVideoDecoderFactory(eglBase!!.eglBaseContext))
            .setAudioDeviceModule(javaAudioDeviceModule!!)
            .createPeerConnectionFactory()
        
        // Create media stream
        localMediaStream = peerConnectionFactory!!.createLocalMediaStream("ARDAMS")
        
        // Create video source and track (for camera frames)
        videoSource = peerConnectionFactory!!.createVideoSource(false)
        videoTrack = peerConnectionFactory!!.createVideoTrack("ARDAMSv0", videoSource!!)
        localMediaStream!!.addTrack(videoTrack!!)
        
        // Create audio source and track using WebRTC's built-in audio processing
        // The audio will be captured by JavaAudioDeviceModule via our custom AudioCapturer
        audioSource = peerConnectionFactory!!.createAudioSource(MediaConstraints())
        audioTrack = peerConnectionFactory!!.createAudioTrack("ARDAMSa0", audioSource!!)
        localMediaStream!!.addTrack(audioTrack!!)
        
        // Create SurfaceTextureHelper for camera -> WebRTC bridge
        surfaceTextureHelper = SurfaceTextureHelper.create("CameraTexture", eglBase!!.eglBaseContext)
        cameraSurfaceTexture = surfaceTextureHelper!!.surfaceTexture
        cameraSurface = Surface(cameraSurfaceTexture!!)
        cameraSurfaceTexture!!.setDefaultBufferSize(VIDEO_WIDTH, VIDEO_HEIGHT)
        
        // Set up listener to forward camera frames to WebRTC video source
        surfaceTextureHelper!!.setListener(object : SurfaceTextureHelper.OnTextureFrameAvailableListener {
            override fun onTextureFrameAvailable(handler: SurfaceTextureHelper) {
                handler.deliverTextureFrame()
            }
        })
        
        // The videoSource will receive frames from SurfaceTextureHelper automatically
        // when we call deliverTextureFrame() which invokes onFrame on all sinks
        videoSource?.addSink(CameraVideoSink())
    }
    
    private fun startCamera() {
        coroutineScope?.launch {
            try {
                val cameraProviderFuture: ListenableFuture<ProcessCameraProvider> = 
                    ProcessCameraProvider.getInstance(this@StreamingService)
                
                val cameraProvider = cameraProviderFuture.get()
                
                // Configure Preview for streaming (feeds frames to Surface)
                preview = Preview.Builder()
                    .setTargetResolution(Size(VIDEO_WIDTH, VIDEO_HEIGHT))
                    .setTargetFrameRate(androidx.camera.core.Range(VIDEO_FPS, VIDEO_FPS))
                    .setTargetRotation(getScreenOrientation())
                    .build()
                
                // Also configure VideoCapture for recording capability (optional)
                val recorder = Recorder.Builder()
                    .setQualitySelector(androidx.camera.video.QualitySelector.from(
                        androidx.camera.video.Quality.HD
                    ))
                    .build()
                videoCapture = recorder
                
                // Use CameraSelector for back camera
                val cameraSelector = CameraSelector.DEFAULT_BACK_CAMERA
                
                // Bind preview to our Surface (which feeds WebRTC via SurfaceTexture)
                preview!!.setSurfaceProvider { surfaceRequest ->
                    surfaceRequest.provideSurface(cameraSurface!!, ContextCompat.getMainExecutor(this@StreamingService))
                }
                
                // Bind to lifecycle (service doesn't have lifecycle, use custom)
                cameraProvider.bindToLifecycle(
                    this@StreamingService, 
                    cameraSelector, 
                    preview!!,
                    videoCapture!!
                )
                
                Log.d(TAG, "Camera started successfully")
                
            } catch (e: Exception) {
                Log.e(TAG, "Failed to start camera", e)
            }
        }
    }
    
    private fun getScreenOrientation(): Int {
        val windowManager = getSystemService(Context.WINDOW_SERVICE) as android.view.WindowManager
        val rotation = windowManager.defaultDisplay.rotation
        return when (rotation) {
            android.view.Surface.ROTATION_0 -> 0
            android.view.Surface.ROTATION_90 -> 90
            android.view.Surface.ROTATION_180 -> 180
            android.view.Surface.ROTATION_270 -> 270
            else -> 0
        }
    }
    
    private fun startAudioCapture() {
        // Initialize WebRTC audio capturer using JavaAudioDeviceModule
        audioCapturer = AndroidAudioCapturer(this, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS)
        
        // Set the custom capturer on the JavaAudioDeviceModule
        javaAudioDeviceModule?.setAudioCapturer(audioCapturer!!)
        
        // Start capturing audio
        audioCapturer?.startCapture()
        
        Log.d(TAG, "Audio capture started via WebRTC JavaAudioDeviceModule")
    }
    
    private fun stopAudioCapture() {
        audioCapturer?.stopCapture()
        audioCapturer = null
        
        // Clear the capturer from the audio device module
        javaAudioDeviceModule?.setAudioCapturer(null)
        
        Log.d(TAG, "Audio capture stopped")
    }
    
    // Custom AudioCapturer implementation for WebRTC
    // This bridges Android AudioRecord to WebRTC's audio pipeline
    private inner class AndroidAudioCapturer(
        private val context: Context,
        private val sampleRate: Int,
        private val channels: Int
    ) : AudioCapturer {
        
        private var audioRecord: AudioRecord? = null
        private val isRunning = AtomicBoolean(false)
        private var captureThread: Thread? = null
        
        private val bufferSize: Int by lazy {
            AudioRecord.getMinBufferSize(
                sampleRate,
                if (channels == 2) AudioFormat.CHANNEL_IN_STEREO else AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT
            ).coerceAtLeast(4096)
        }
        
        override fun startCapture() {
            if (isRunning.getAndSet(true)) return
            
            val channelConfig = if (channels == 2) AudioFormat.CHANNEL_IN_STEREO else AudioFormat.CHANNEL_IN_MONO
            
            audioRecord = AudioRecord(
                MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                sampleRate,
                channelConfig,
                AudioFormat.ENCODING_PCM_16BIT,
                bufferSize
            )
            
            if (audioRecord!!.state != AudioRecord.STATE_INITIALIZED) {
                Log.e(TAG, "AudioRecord failed to initialize in AndroidAudioCapturer")
                isRunning.set(false)
                return
            }
            
            audioRecord!!.startRecording()
            
            captureThread = Thread("WebRTC-AudioCapturer") {
                val buffer = ByteBuffer.allocateDirect(bufferSize)
                
                while (isRunning.get()) {
                    val readResult = audioRecord?.read(buffer, bufferSize, AudioRecord.READ_BLOCKING) ?: 0
                    
                    if (readResult > 0) {
                        buffer.rewind()
                        // Convert to WebRTC AudioFrame format and deliver
                        // WebRTC expects 10ms frames at the sample rate
                        val samplesPer10ms = sampleRate / 100 // 480 samples at 48kHz
                        val bytesPerSample = 2 // 16-bit PCM
                        val frameSize = samplesPer10ms * channels * bytesPerSample
                        
                        // Deliver audio data to WebRTC via the capturer callback
                        // The JavaAudioDeviceModule will handle the conversion and deliver to AudioTrack
                        onCaptureData(buffer, readResult, sampleRate, channels)
                    } else if (readResult < 0) {
                        Log.e(TAG, "AudioRecord read error: $readResult")
                    }
                }
            }.apply { start() }
            
            Log.d(TAG, "AndroidAudioCapturer started")
        }
        
        override fun stopCapture() {
            if (!isRunning.getAndSet(false)) return
            
            captureThread?.interrupt()
            try {
                captureThread?.join(1000)
            } catch (e: InterruptedException) {
                Thread.currentThread().interrupt()
            }
            captureThread = null
            
            audioRecord?.stop()
            audioRecord?.release()
            audioRecord = null
            
            Log.d(TAG, "AndroidAudioCapturer stopped")
        }
        
        override fun getAudioParameters(): AudioParameters {
            return AudioParameters(
                AudioParameters.AudioFormat.PCM_16BIT,
                sampleRate,
                channels
            )
        }
        
        override fun getSupportedAudioParameters(): MutableList<AudioParameters> {
            val params = mutableListOf<AudioParameters>()
            params.add(AudioParameters(
                AudioParameters.AudioFormat.PCM_16BIT,
                sampleRate,
                channels
            ))
            return params
        }
    }
    
    private fun createPeerConnection(): PeerConnection? {
        val iceServers = mutableListOf<PeerConnection.IceServer>()
        // Add STUN servers
        iceServers.add(PeerConnection.IceServer("stun:stun.l.google.com:19302"))
        iceServers.add(PeerConnection.IceServer("stun:stun1.l.google.com:19302"))
        // TURN servers would be added here with credentials
        
        val rtcConfig = PeerConnection.RTCConfiguration(iceServers)
        rtcConfig.tcpCandidatePolicy = PeerConnection.TcpCandidatePolicy.DISABLED
        rtcConfig.bundlePolicy = PeerConnection.BundlePolicy.MAXBUNDLE
        rtcConfig.rtcpMuxPolicy = PeerConnection.RtcpMuxPolicy.REQUIRE
        rtcConfig.continualGatheringPolicy = PeerConnection.ContinualGatheringPolicy.GATHER_CONTINUALLY
        rtcConfig.iceTransportsType = PeerConnection.IceTransportsType.ALL
        rtcConfig.keyType = PeerConnection.KeyType.ECDSA
        
        val constraints = MediaConstraints()
        constraints.mandatory.add(MediaConstraints.KeyValuePair("OfferToReceiveAudio", "true"))
        constraints.mandatory.add(MediaConstraints.KeyValuePair("OfferToReceiveVideo", "true"))
        constraints.optional.add(MediaConstraints.KeyValuePair("DtlsSrtpKeyAgreement", "true"))
        
        return peerConnectionFactory!!.createPeerConnection(rtcConfig, constraints, object : PeerConnection.Observer {
            override fun onSignalingChange(newState: PeerConnection.SignalingState) {
                Log.d(TAG, "Signaling state: $newState")
            }
            
            override fun onIceConnectionChange(newState: PeerConnection.IceConnectionState) {
                Log.d(TAG, "ICE connection state: $newState")
                if (newState == PeerConnection.IceConnectionState.CONNECTED) {
                    Log.i(TAG, "WebRTC connected!")
                }
            }
            
            override fun onIceConnectionReceivingChange(receiving: Boolean) {}
            
            override fun onIceGatheringChange(newState: PeerConnection.IceGatheringState) {
                Log.d(TAG, "ICE gathering state: $newState")
            }
            
            override fun onIceCandidate(candidate: IceCandidate) {
                Log.d(TAG, "ICE candidate: ${candidate.sdp}")
                // Send candidate to signaling server
                sendIceCandidate(candidate)
            }
            
            override fun onIceCandidatesRemoved(candidates: Array<IceCandidate>) {}
            
            override fun onAddStream(stream: MediaStream) {}
            
            override fun onRemoveStream(stream: MediaStream) {}
            
            override fun onDataChannel(dataChannel: org.webrtc.DataChannel) {}
            
            override fun onRenegotiationNeeded() {
                createOffer()
            }
            
            override fun onAddTrack(rtpTransceiver: RtpTransceiver) {}
        })
    }
    
    private fun createOffer() {
        peerConnection?.createOffer(object : PeerConnection.SdpObserver {
            override fun onCreateSuccess(sessionDescription: SessionDescription) {
                peerConnection?.setLocalDescription(this, sessionDescription)
                Log.d(TAG, "Created offer: ${sessionDescription.description}")
                // Send offer to signaling server
                sendOffer(sessionDescription)
            }
            
            override fun onCreateFailure(error: String) {
                Log.e(TAG, "Failed to create offer: $error")
            }
            
            override fun onSetSuccess() {
                Log.d(TAG, "Set local description success")
            }
            
            override fun onSetFailure(error: String) {
                Log.e(TAG, "Set local description failed: $error")
            }
        }, MediaConstraints())
    }
    
    private fun sendOffer(sessionDescription: SessionDescription) {
        // TODO: Implement actual signaling (WebSocket, HTTP, etc.)
        Log.d(TAG, "Would send offer to signaling server")
        signalingConnected = true
    }
    
    private fun sendIceCandidate(candidate: IceCandidate) {
        // TODO: Implement actual signaling
        Log.d(TAG, "Would send ICE candidate to signaling server")
    }
    
    private fun startWebRTC() {
        peerConnection = createPeerConnection()
        
        if (peerConnection != null) {
            // Add local tracks
            localMediaStream?.getVideoTracks()?.forEach { track ->
                peerConnection!!.addTrack(track, arrayOf("ARDAMS"))
            }
            localMediaStream?.getAudioTracks()?.forEach { track ->
                peerConnection!!.addTrack(track, arrayOf("ARDAMS"))
            }
            
            // Create offer to start negotiation
            createOffer()
        }
    }
    
    private fun stopCamera() {
        preview?.setSurfaceProvider(null)
        videoCapture = null
        preview = null
    }
    
    private fun stopAudioCapture() {
        audioCapturer?.stopCapture()
        audioCapturer = null
        
        // Clear the capturer from the audio device module
        javaAudioDeviceModule?.setAudioCapturer(null)
        
        Log.d(TAG, "Audio capture stopped")
    }
    
    private fun stopWebRTC() {
        peerConnection?.close()
        peerConnection = null
        
        videoTrack?.dispose()
        videoTrack = null
        videoSource?.dispose()
        videoSource = null
        
        audioTrack?.dispose()
        audioTrack = null
        audioSource?.dispose()
        audioSource = null
        
        localMediaStream?.dispose()
        localMediaStream = null
        
        surfaceTextureHelper?.dispose()
        surfaceTextureHelper = null
        cameraSurfaceTexture = null
        cameraSurface?.release()
        cameraSurface = null
    }
    
    private fun checkPermissions(): Boolean {
        val cameraPermission = ContextCompat.checkSelfPermission(this, android.Manifest.permission.CAMERA)
        val audioPermission = ContextCompat.checkSelfPermission(this, android.Manifest.permission.RECORD_AUDIO)
        
        return cameraPermission == PackageManager.PERMISSION_GRANTED && 
               audioPermission == PackageManager.PERMISSION_GRANTED
    }
    
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Rule 8: Runtime permission guard before starting foreground
        if (!checkPermissions()) {
            Log.e(TAG, "Missing required permissions (CAMERA, RECORD_AUDIO)")
            stopSelf()
            return START_NOT_STICKY
        }
        
        val notification = Notification.Builder(this, "phonecam_stream")
            .setContentTitle("PhoneCam streaming")
            .setContentText("Camera and microphone are active")
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setOngoing(true)
            .build()
        
        startForeground(10, notification)
        
        // Start camera, audio, and WebRTC
       startCamera()
       startAudioCapture()
       startWebRTC()
        
        return START_STICKY
    }
    
    override fun onDestroy() {
        super.onDestroy()
        
        // Stop all components
       stopCamera()
       stopAudioCapture()
       stopWebRTC()
        
        // Cleanup WebRTC
        peerConnectionFactory?.dispose()
        peerConnectionFactory = null
        eglBase?.release()
        eglBase = null
        
        // Cleanup coroutines
        mainJob?.cancel()
        coroutineScope = null
        
        // Cleanup executors
        cameraExecutor?.shutdown()
        try {
            cameraExecutor?.awaitTermination(2, TimeUnit.SECONDS)
        } catch (e: InterruptedException) {
            Thread.currentThread().interrupt()
        }
        cameraExecutor = null
    }
    
    override fun onBind(intent: Intent?): IBinder? = null
    
    // VideoSink to receive frames from CameraX Preview via SurfaceTextureHelper
    // and push them to WebRTC VideoSource
    private inner class CameraVideoSink : VideoSink {
        override fun onFrame(frame: VideoFrame) {
            // This is called when SurfaceTextureHelper delivers a frame
            // The frame is already in the correct format for WebRTC
            videoSource?.onFrame(frame)
        }
    }
}