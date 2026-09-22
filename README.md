# PhoneCam Stream — MVP

Phone camera → Wi-Fi → PC browser receiver, designed for OBS workflows.

## Architecture
- Android native Kotlin app using WebRTC and a camera foreground service.
- PC receiver is a tiny Node.js WebSocket signaling server + browser WebRTC viewer.
- The Android service uses the Android camera foreground-service type so streaming can continue after the display is turned off, subject to device/OEM policies.

## Android
Open `android/` in Android Studio/VS Code with the Android SDK installed. Build/install the app, enter the PC's LAN signaling URL shown by the receiver, grant camera/microphone permissions, and press START STREAM while the app is visible. Android requires camera foreground services to be started while the app is visible on modern versions.

## PC receiver
From `pc-receiver/`:

```bash
npm install
npm start
```

Open `http://localhost:8765` on the PC. The terminal prints the LAN WebSocket URL to enter in the phone app.

## OBS
For the first MVP, use OBS Browser Source pointed at `http://localhost:8765`. A later iteration can add a native OBS source/virtual camera bridge and expose bitrate/FPS/quality controls.

## Current MVP defaults
- Rear camera
- 1920×1080 @ 30 FPS requested
- Hardware WebRTC encoder when available
- STUN for ICE
- Screen-off-capable foreground service
- Partial wake lock while actively streaming

Device-specific thermal throttling and battery optimization can still affect long sessions.
