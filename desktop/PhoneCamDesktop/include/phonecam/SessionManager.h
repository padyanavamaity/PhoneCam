#pragma once
#include <string>
#include <unordered_map>
namespace phonecam {
struct CameraSession { std::string deviceId; std::string sessionId; bool videoEnabled=true; bool audioEnabled=true; bool muted=false; float gain=1.0f; int audioDelayMs=0; };
class SessionManager {
public: bool addSession(const CameraSession&); bool removeSession(const std::string&); bool setAudioEnabled(const std::string&, bool); bool setMuted(const std::string&, bool); bool setVideoEnabled(const std::string&, bool); bool setGain(const std::string&, float);
private: std::unordered_map<std::string, CameraSession> sessions_;
};
}
