#include "phonecam/SessionManager.h"
namespace phonecam {
bool SessionManager::addSession(const CameraSession& s){ return !s.deviceId.empty() && !s.sessionId.empty() && sessions_.emplace(s.deviceId,s).second; }
bool SessionManager::removeSession(const std::string& id){ return sessions_.erase(id)>0; }
bool SessionManager::setAudioEnabled(const std::string& id,bool v){ auto i=sessions_.find(id); if(i==sessions_.end()) return false; i->second.audioEnabled=v; return true; }
bool SessionManager::setMuted(const std::string& id,bool v){ auto i=sessions_.find(id); if(i==sessions_.end()) return false; i->second.muted=v; return true; }
bool SessionManager::setVideoEnabled(const std::string& id,bool v){ auto i=sessions_.find(id); if(i==sessions_.end()) return false; i->second.videoEnabled=v; return true; }
bool SessionManager::setGain(const std::string& id,float g){ if(g<0||g>4) return false; auto i=sessions_.find(id); if(i==sessions_.end()) return false; i->second.gain=g; return true; }
}
