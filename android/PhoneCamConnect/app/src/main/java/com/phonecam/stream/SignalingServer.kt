package com.phonecam.stream

import android.util.Log
import org.java_websocket.WebSocket
import org.java_websocket.handshake.ClientHandshake
import org.java_websocket.server.WebSocketServer
import org.json.JSONObject
import java.net.InetSocketAddress

/**
 * WebSocket signaling endpoint hosted by the phone (spec §12).
 * Carries ONLY session setup + control messages, never media.
 * Binds an ephemeral port; the port is advertised through mDNS.
 *
 * One desktop controller at a time; a newer connection replaces the older one
 * (lets a desktop reconnect quickly after a network blip).
 *
 * SECURITY (spec §27): not implemented yet. Any LAN client can connect.
 * Authorize in Callbacks.onClientConnected / message handling once pairing exists.
 */
class SignalingServer(private val callbacks: Callbacks) : WebSocketServer(InetSocketAddress(0)) {

    interface Callbacks {
        fun onServerStarted(port: Int)
        fun onServerError(e: Exception)
        fun onClientConnected(remote: String)
        fun onClientMessage(msg: JSONObject)
        fun onClientDisconnected()
    }

    @Volatile private var client: WebSocket? = null

    init {
        setReuseAddr(true)
        setConnectionLostTimeout(15)   // heartbeat: detects a vanished desktop
    }

    fun send(json: JSONObject) {
        try {
            client?.takeIf { it.isOpen }?.send(json.toString())
        } catch (e: Exception) {
            Log.w(TAG, "send failed", e)
        }
    }

    fun disconnectClient() {
        client?.close(1000, "bye")
    }

    override fun onStart() {
        callbacks.onServerStarted(port)
    }

    override fun onOpen(conn: WebSocket?, handshake: ClientHandshake?) {
        conn ?: return
        val old = client
        client = conn
        if (old != null && old !== conn && old.isOpen) old.close(4002, "replaced")
        callbacks.onClientConnected(conn.remoteSocketAddress?.address?.hostAddress ?: "unknown")
    }

    override fun onClose(conn: WebSocket?, code: Int, reason: String?, remote: Boolean) {
        if (conn != null && conn === client) {
            client = null
            callbacks.onClientDisconnected()
        }
    }

    override fun onMessage(conn: WebSocket?, message: String?) {
        if (conn == null || conn !== client || message == null) return
        try {
            callbacks.onClientMessage(JSONObject(message))
        } catch (e: Exception) {
            Log.w(TAG, "bad message: $message", e)
        }
    }

    override fun onError(conn: WebSocket?, ex: Exception?) {
        if (ex == null) return
        if (conn == null) callbacks.onServerError(ex) else Log.w(TAG, "socket error", ex)
    }

    companion object { private const val TAG = "SignalingServer" }
}