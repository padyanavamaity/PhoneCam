# Audio Architecture

Every newly started camera stream sends microphone audio by default after microphone permission is granted.

Each phone remains independent:

```text
CAM1 -> MIC1
CAM2 -> MIC2
CAM3 -> MIC3
```

The desktop must not irreversibly mix these feeds. It exposes per-camera controls and can pass independent audio to OBS.

Per-camera controls:
- enable/disable
- mute/unmute
- gain
- delay
- monitor
- peak level
- clipping indicator

Global controls:
- mute all
- unmute all
- master level
