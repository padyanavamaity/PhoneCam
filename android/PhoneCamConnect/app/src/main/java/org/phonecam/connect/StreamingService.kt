package org.phonecam.connect

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.IBinder

class StreamingService : Service() {
    override fun onCreate() {
        super.onCreate()
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(NotificationChannel("phonecam_stream", "PhoneCam streaming", NotificationManager.IMPORTANCE_LOW))
        // TODO: integrate Camera2/CameraX, AudioRecord and native WebRTC.
        // The production implementation must add one video and one audio track.
    }
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = Notification.Builder(this, "phonecam_stream")
            .setContentTitle("PhoneCam streaming")
            .setContentText("Camera and microphone are active")
            .setSmallIcon(android.R.drawable.ic_menu_camera).build()
        startForeground(10, notification)
        return START_STICKY
    }
    override fun onBind(intent: Intent?): IBinder? = null
}
