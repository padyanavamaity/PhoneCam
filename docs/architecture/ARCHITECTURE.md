# Architecture

## Android

`PhoneCam Connect` captures camera video and microphone audio, creates a WebRTC peer session, and runs the active capture in a foreground service. Screen-off operation is subject to Android permissions, foreground-service rules and OEM behavior.

## Desktop

`PhoneCam Desktop` owns sessions, signaling, WebRTC receiving, decoding, frame/audio distribution, UI controls and diagnostics.

A session is isolated by `deviceId + sessionId`. Commands are scoped to that session.

## OBS

`PhoneCam OBS` is intended to provide one OBS source per phone session. Video and audio remain independent until OBS or an explicitly configured mixer combines them.

## Control commands

- VIDEO_ENABLE / VIDEO_DISABLE
- AUDIO_ENABLE / AUDIO_DISABLE
- AUDIO_MUTE / AUDIO_UNMUTE
- AUDIO_GAIN
- AUDIO_DELAY
- CAMERA_SELECT
- TORCH
- ZOOM
- RESOLUTION
- FPS
- BITRATE
- RECONNECT
- DISCONNECT
