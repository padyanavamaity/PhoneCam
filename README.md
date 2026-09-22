# PhoneCam

Multi-camera Android camera + microphone streaming system for a Windows production desktop and OBS.

**Status:** engineering foundation/scaffold. The architecture and interfaces are defined, but native WebRTC, device-specific camera behavior, decoder integration, installer signing, and final OBS SDK integration still require implementation/testing before production use.

## Architecture

Android phones publish independent WebRTC video + audio tracks to PhoneCam Desktop. Desktop manages every feed and exposes each feed independently to OBS.

```text
Phone 1 ─┐
Phone 2 ─┼─ WebRTC ─> PhoneCam Desktop Core ─> OBS Plugin ─> OBS
Phone 3 ─┘
```

## Audio

Microphone transmission is enabled by default for a new stream after explicit Android microphone permission. Each phone has an independent audio track. Desktop controls audio per camera and globally: mute, gain, delay, monitoring and peak/clipping state.

## Security

Pairing, authentication, session authorization, protocol validation, secure secret storage, local-only OBS IPC, secret-free logs and reconnect identity verification are mandatory design goals. See `docs/security/THREAT_MODEL.md`.

## Miscellaneous

`Miscellaneous/` contains plans, AI prompts, experiments and other working material. It is intentionally listed in the ZIP but ignored by Git via `.gitignore`, so it is available to AI coding agents locally without being committed.

**Do not put secrets in Miscellaneous. Gitignored is not the same as secure.**
