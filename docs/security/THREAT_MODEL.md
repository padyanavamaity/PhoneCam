# Security Threat Model

## Assets

Camera video, microphone audio, pairing credentials, device identity, session metadata and OBS control access.

## Required mitigations

### Unauthorized LAN device
Explicit pairing, short-lived pairing credential, persistent device identity and session authorization.

### Command injection
Typed protocol, schema/range validation, session ownership checks and rejection of unknown commands.

### Credential leakage
No hard-coded secrets; Android Keystore / Windows protected storage for persistent secrets; redact sensitive logs.

### OBS control exposure
Use authenticated local IPC/named-pipe style transport with local access controls. Do not expose OBS control over the LAN.

### Malformed signaling
Validate SDP/ICE and protocol messages, enforce size limits, timeouts and rate limits.

### Privacy leakage
No recording or cloud upload by default. Clear camera/microphone activity indication.

### Reconnection hijacking
A reconnect must prove the existing device identity. IP address alone is never identity.

### Supply-chain risk
Pin dependencies where practical, review release artifacts, sign production builds, and keep temporary AI material under the gitignored `Miscellaneous/` directory.

## Security acceptance tests

- Unauthorized pairing rejected.
- Cross-device commands rejected.
- Unknown commands rejected.
- Oversized payloads rejected.
- Stale/replayed control messages rejected.
- Secrets absent from logs.
- OBS IPC inaccessible to unauthorized local clients.
- Reconnect identity verified.
- Protocol fuzz/negative tests exist.
