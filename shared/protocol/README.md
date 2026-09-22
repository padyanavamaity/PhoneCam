# Protocol Contract

Every control message should contain:

- protocolVersion
- messageId
- sessionId
- deviceId
- timestamp
- command
- payload

The receiver must reject unknown versions, oversized payloads, invalid ranges, stale messages and messages whose device/session relationship is invalid.
