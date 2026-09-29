// WebRtcReceiver.cpp
//
// Real WebRTC answerer on top of libdatachannel. Design notes:
//
// * Android (phone) is ALWAYS the SDP offerer. Desktop is ALWAYS the answerer.
//   The phone sends a "sessionInit" + "offer" over plain TCP/JSON; desktop
//   parses that offer and synthesizes an answer via libdatachannel's
//   PeerConnection and its pre-added RECVONLY video and audio m-lines.
//
// * We force VP8 (payload type 96) by adding only `rtc::Description::Video`
//   with `addVP8Codec(96)`. libdatachannel will pick VP8 for the answer even
//   if the offer prefers H264/H265, as long as the offer itself contains VP8
//   (which it does -- Android webrtc.org SDK always offers VP8 among others).
//
// * We force Opus (payload type 111) by adding `rtc::Description::Audio`
//   with `addOpusCodec(111)`. Android webrtc.org SDK always offers Opus.
//
// * Empty ICE servers list. Both ends are on the LAN and we only ever use
//   host candidates. This matches PeerConnectionFactory on Android where
//   app passes an EMPTY ICE server list.
//
// * Per-session state is kept inside a WebRtcSession struct owned by the
//   sessions_ map. The PeerConnection itself is held as
//   shared_ptr<rtc::PeerConnection>.
//
// * When VPX decode is unavailable (PHONECAM_DEPAY_ONLY), the depayloader
//   still produces a WebRtcVideoFrame whose `data` carries the raw VP8
//   bitstream, `codec="VP8"`, and width=height=0. The preview window can
//   then display REAL state information (frame counter, byte size, RTP
//   timestamp) without decoding pixels -- never fabricated values.
//
// * When Opus decode is unavailable (PHONECAM_OPUS_DEPAY_ONLY), the depayloader
//   still produces a WebRtcAudioFrame whose `data` carries the raw Opus
//   bitstream, `codec="opus"`, sampleRate=48000, channels=2. Consumers can
//   decode externally or pass through to OBS.

#include "phonecam/WebRtcReceiver.h"

#include "../media/Vp8Depayloader.h"
#include "../media/OpusDepayloader.h"

#ifdef PHONECAM_HAVE_VPX
#  include "../media/Vp8Decoder.h"
#endif

#ifdef PHONECAM_HAVE_OPUS
#  include "../media/OpusDecoder.h"
#endif

#include <rtc/rtc.hpp>

#include <iostream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace phonecam {

// ------------------------------- session -----------------------------------

namespace {

struct PerSession {
    std::string sessionId;
    std::string deviceId;

    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> videoTrack;
    std::shared_ptr<rtc::Track> audioTrack;

    Vp8Depayloader videoDepay;
    OpusDepayloader audioDepay;

#ifdef PHONECAM_HAVE_VPX
    std::unique_ptr<Vp8Decoder> videoDecoder;
#endif

#ifdef PHONECAM_HAVE_OPUS
    std::unique_ptr<OpusDecoder> audioDecoder;
#endif

    std::atomic<bool> videoEnabled{true};
    std::atomic<bool> audioEnabled{false};
    std::atomic<rtc::PeerConnection::State> pcState{rtc::PeerConnection::State::New};

    std::string localAnswerSdp;
    bool haveLocalAnswer = false;

    std::mutex mutex; // guards non-atomic state above
};

const char* toString(rtc::PeerConnection::State s) {
    switch (s) {
        case rtc::PeerConnection::State::New:          return "new";
        case rtc::PeerConnection::State::Connecting:   return "connecting";
        case rtc::PeerConnection::State::Connected:    return "connected";
        case rtc::PeerConnection::State::Disconnected: return "disconnected";
        case rtc::PeerConnection::State::Failed:       return "failed";
        case rtc::PeerConnection::State::Closed:       return "closed";
    }
    return "unknown";
}

} // namespace

// ------------------------------ implementation -----------------------------

class WebRtcReceiverImpl : public WebRtcReceiver {
public:
    WebRtcReceiverImpl() = default;
    ~WebRtcReceiverImpl() override { shutdown(); }

    bool initialize() override {
        if (initialized_.exchange(true)) return true;
        rtc::InitLogger(rtc::LogLevel::Warning);
        // rtc::Preload() is implicitly invoked by PeerConnection creation.
        // We don't enable separate loggers here to keep output tidy.
        std::cout << "[WebRtcReceiver] initialized (libdatachannel)" << std::endl;
        return true;
    }

    bool createSession(const WebRtcSessionConfig& config) override {
        if (!initialized_.load()) {
            std::cerr << "[WebRtcReceiver] not initialized" << std::endl;
            return false;
        }
        if (config.sessionId.empty() || config.deviceId.empty()) {
            std::cerr << "[WebRtcReceiver] invalid session config" << std::endl;
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (sessions_.find(config.sessionId) != sessions_.end()) {
                std::cerr << "[WebRtcReceiver] session already exists " << config.sessionId << std::endl;
                return false;
            }
        }

        // Create libdatachannel PeerConnection.
        rtc::Configuration cfg;
        // LAN-only: no STUN/TURN.
        cfg.iceServers.clear();
        cfg.disableAutoNegotiation = true;       // We drive setRemote/setLocal ourselves.
        cfg.forceMediaTransport = true;          // We don't use datachannels.
        cfg.portRangeBegin = 0;
        cfg.portRangeEnd = 0;

        auto pc = std::make_shared<rtc::PeerConnection>(cfg);

        auto sess = std::make_shared<PerSession>();
        sess->sessionId = config.sessionId;
        sess->deviceId = config.deviceId;
        sess->pc = pc;
#ifdef PHONECAM_HAVE_VPX
        sess->videoDecoder = std::make_unique<Vp8Decoder>();
#endif
#ifdef PHONECAM_HAVE_OPUS
        sess->audioDecoder = std::make_unique<OpusDecoder>();
#endif

        // Pre-add a RECVONLY video track so libdatachannel will ready a video
        // m-line to match against the offer's video m-line. We ONLY add VP8
        // to force the codec choice.
        rtc::Description::Video v("video", rtc::Description::Direction::RecvOnly);
        v.addVP8Codec(96);

        auto track = pc->addTrack(v);
        if (!track) {
            std::cerr << "[WebRtcReceiver] failed to add video track" << std::endl;
            return false;
        }
        sess->videoTrack = track;

        // Pre-add a RECVONLY audio track so libdatachannel will ready an audio
        // m-line to match against the offer's audio m-line. We ONLY add Opus
        // to force the codec choice (payload type 111 is standard for Opus).
        rtc::Description::Audio a("audio", rtc::Description::Direction::RecvOnly);
        a.addOpusCodec(111);

        auto audioTrack = pc->addTrack(a);
        if (!audioTrack) {
            std::cerr << "[WebRtcReceiver] failed to add audio track" << std::endl;
            return false;
        }
        sess->audioTrack = audioTrack;

        // ------------------------- state callbacks -------------------------
        std::string sid = config.sessionId;
        pc->onStateChange([this, sid, sess](rtc::PeerConnection::State s) {
            sess->pcState.store(s);
            std::cout << "[WebRtcReceiver] " << sid << " peerConnectionState=" << toString(s) << std::endl;
            notifyState(sid, toString(s));
        });

        pc->onIceStateChange([sid](rtc::PeerConnection::IceState s) {
            std::cout << "[WebRtcReceiver] " << sid << " iceState=" << static_cast<int>(s) << std::endl;
        });

        pc->onGatheringStateChange([sid](rtc::PeerConnection::GatheringState s) {
            std::cout << "[WebRtcReceiver] " << sid << " gatheringState=" << static_cast<int>(s) << std::endl;
        });

        // ------------------------- local description -----------------------
        // We disable auto-negotiation, so onLocalDescription fires once when
        // we explicitly call setLocalDescription(Answer).
        pc->onLocalDescription([this, sid, sess](const rtc::Description& d) {
            // libdatachannel hands back the full SDP (multi-line). Android
            // expects a single JSON string with embedded \r\n; nlohmann_json
            // will escape them correctly when we serialize at SignalingServer.
            std::lock_guard<std::mutex> lk(sess->mutex);
            sess->localAnswerSdp = std::string(d);
            sess->haveLocalAnswer = true;
            std::cout << "[WebRtcReceiver] " << sid << " local answer generated, sdpBytes=" << sess->localAnswerSdp.size() << std::endl;
        });

        // ------------------------- local ICE candidates ---------------------
        pc->onLocalCandidate([this, sid](const rtc::Candidate& c) {
            // libdatachannel emits candidate strings like:
            //   "candidate:1 1 udp 2130706431 192.168.1.7 9 typ host"
            // without the leading "a=" -- exactly what Android's SignalingClient
            // expects in the "candidate" JSON field.
            // The rtc::Candidate provides mid() for proper routing.
            // Note: mlineIndex is not available in this version of libdatachannel,
            // the mid() is used to associate candidates with media streams.
            if (iceCb_) {
                std::cout << "[WebRtcReceiver] " << sid << " local ICE candidate: mid=" << c.mid()
                          << " candidate=" << std::string(c) << std::endl;
                iceCb_(sid, c.mid(), 0, std::string(c));
            }
        });

        // ------------------------- incoming RTP bytes -----------------------
        // Because we did NOT attach a ChainLike media handler (e.g. a
        // rtc::RtpPacketizer or rtc::RtcpReceivingSession), libdatachannel
        // will deliver the raw RTP packet bytes (12-byte RTP header + payload)
        // via onMessage(). We feed them into our VP8/Opus depayloaders.
        track->onMessage([this, sid, sess](rtc::message_variant mv) {
            if (!std::holds_alternative<rtc::binary>(mv)) return;
            const auto& bytes = std::get<rtc::binary>(mv);
            if (bytes.empty()) return;

            const uint8_t* p = reinterpret_cast<const uint8_t*>(bytes.data());
            const size_t n = bytes.size();
            auto vf = sess->videoDepay.pushRtp(p, n);
            if (!vf.has_value()) return;

            // We have a complete VP8 frame -- populate WebRtcVideoFrame.
            WebRtcVideoFrame f;
            f.codec = "VP8";
            f.timestampUs = static_cast<int64_t>(
                (static_cast<uint64_t>(vf->rtpTimestamp) * 1'000'000ULL) / 90'000ULL);
            f.format = 0;   // I420 marker (when we really produce I420)
            f.stride = 0;

#ifdef PHONECAM_HAVE_VPX
            auto decoded = sess->videoDecoder ? sess->videoDecoder->decode(vf->payload.data(),
                                                                          vf->payload.size()) : nullptr;
            if (decoded && decoded->data) {
                f.width = decoded->width;
                f.height = decoded->height;
                f.stride = decoded->width;
                f.data.assign(decoded->data.get(),
                              decoded->data.get() +
                                  (static_cast<size_t>(decoded->width) * decoded->height +
                                   2 * ((decoded->width + 1) / 2) * ((decoded->height + 1) / 2)));
            } else {
                // Decoder failed (corrupt frame or unsupported). Drop it so we
                // never deliver garbage to consumers.
                return;
            }
#else
            // Depay-only mode: deliver the raw VP8 bitstream and let the
            // preview layer surface byte size / RTP timestamp / frame counter
            // (all real data). No pixel decoding.
            f.width = 0;
            f.height = 0;
            f.data = vf->payload;   // copy (vf is const ref from optional)
#endif

            std::cout << "[WebRtcReceiver] " << sid << " video frame received: "
                      << "width=" << f.width << " height=" << f.height
                      << " timestampUs=" << f.timestampUs
                      << " payloadBytes=" << vf->payload.size()
                      << " rtpTimestamp=" << vf->rtpTimestamp << std::endl;

            if (videoCb_) videoCb_(sid, f);
        });

        // Audio track message handler
        sess->audioTrack->onMessage([this, sid, sess](rtc::message_variant mv) {
            if (!std::holds_alternative<rtc::binary>(mv)) return;
            const auto& bytes = std::get<rtc::binary>(mv);
            if (bytes.empty()) return;

            const uint8_t* p = reinterpret_cast<const uint8_t*>(bytes.data());
            const size_t n = bytes.size();
            auto af = sess->audioDepay.pushRtp(p, n);
            if (!af.has_value()) return;

            // We have a complete Opus frame -- populate WebRtcAudioFrame.
            WebRtcAudioFrame f;
            f.codec = "opus";
            f.sampleRate = 48000;
            f.channels = 2;
            f.timestampUs = static_cast<int64_t>(
                (static_cast<uint64_t>(af->rtpTimestamp) * 1'000'000ULL) / 48'000ULL);
            f.frames = 0;  // Will be set after decode if available

#ifdef PHONECAM_HAVE_OPUS
            auto decoded = sess->audioDecoder ? sess->audioDecoder->decode(af->payload.data(),
                                                                           af->payload.size()) : nullptr;
            if (decoded && !decoded->samples.empty()) {
                f.frames = decoded->frames;
                f.data = std::move(decoded->samples);  // interleaved float samples
                f.codec = "PCM";
            } else {
                // Decoder failed or DTX frame. Deliver raw Opus bitstream.
                f.frames = 0;  // marker for raw Opus
                f.data.resize(4 + af->payload.size());
                uint32_t payloadSize = static_cast<uint32_t>(af->payload.size());
                std::memcpy(f.data.data(), &payloadSize, 4);
                std::memcpy(f.data.data() + 4, af->payload.data(), af->payload.size());
                f.codec = "opus";
            }
#else
            // Depay-only mode: deliver the raw Opus bitstream.
            // We encode the raw Opus payload as a special format:
            // first 4 bytes = payload size (uint32_t), followed by Opus data
            // Consumers can detect this by checking if frames==0 and data.size() >= 4
            f.frames = 0;  // marker for raw Opus
            f.data.resize(4 + af->payload.size());
            uint32_t payloadSize = static_cast<uint32_t>(af->payload.size());
            std::memcpy(f.data.data(), &payloadSize, 4);
            std::memcpy(f.data.data() + 4, af->payload.data(), af->payload.size());
            f.codec = "opus";
#endif

            std::cout << "[WebRtcReceiver] " << sid << " audio frame received: "
                      << "codec=" << f.codec << " sampleRate=" << f.sampleRate
                      << " channels=" << f.channels << " frames=" << f.frames
                      << " timestampUs=" << f.timestampUs
                      << " payloadBytes=" << af->payload.size()
                      << " rtpTimestamp=" << af->rtpTimestamp << std::endl;

            if (audioCb_) audioCb_(sid, f);
        });

        {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_[config.sessionId] = std::move(sess);
        }
        std::cout << "[WebRtcReceiver] Session created: " << config.sessionId << " for device: " << config.deviceId << std::endl;
        notifyState(config.sessionId, "new");
        return true;
    }

    bool removeSession(const std::string& sessionId) override {
        std::shared_ptr<PerSession> sess;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(sessionId);
            if (it == sessions_.end()) {
                std::cerr << "[WebRtcReceiver] removeSession: no session " << sessionId << std::endl;
                return false;
            }
            sess = it->second;
            sessions_.erase(it);
        }
        try {
            if (sess->pc) sess->pc->close();
        } catch (...) {
            // never throw from close()
        }
        std::cout << "[WebRtcReceiver] Session removed: " << sessionId << std::endl;
        notifyState(sessionId, "closed");
        return true;
    }

    bool setRemoteOffer(const std::string& sessionId, const std::string& sdp) override {
        std::shared_ptr<PerSession> sess;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(sessionId);
            if (it == sessions_.end()) {
                std::cerr << "[WebRtcReceiver] setRemoteOffer: no session " << sessionId << std::endl;
                return false;
            }
            sess = it->second;
        }
        std::cout << "[WebRtcReceiver] " << sessionId << " setRemoteOffer: sdpBytes=" << sdp.size() << std::endl;
        try {
            sess->pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Offer));
            // Since disableAutoNegotiation=true, we must explicitly ask
            // libdatachannel to produce the Answer.
            sess->pc->setLocalDescription(rtc::Description::Type::Answer);
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[WebRtcReceiver] setRemoteOffer failed: " << e.what() << std::endl;
            return false;
        }
    }

    bool setRemoteAnswer(const std::string& /*sessionId*/, const std::string& /*sdp*/) override {
        // Desktop never offers in PhoneCam. See header for design rationale.
        std::cerr << "[WebRtcReceiver] setRemoteAnswer called but desktop is NEVER the offerer"
                     " -- ignoring (treat as programming error)" << std::endl;
        return false;
    }

    bool addIceCandidate(const std::string& sessionId, const std::string& sdpMid, int /*sdpMLineIndex*/, const std::string& candidate) override {
        std::shared_ptr<PerSession> sess;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(sessionId);
            if (it == sessions_.end()) {
                std::cerr << "[WebRtcReceiver] addIceCandidate: no session " << sessionId << std::endl;
                return false;
            }
            sess = it->second;
        }
        std::cout << "[WebRtcReceiver] " << sessionId << " addIceCandidate: mid=" << sdpMid
                  << " candidateBytes=" << candidate.size() << std::endl;
        try {
            // Use the sdpMid provided by the signaling layer
            // to properly route the candidate to the correct m-line.
            // Note: mlineIndex is not available in this version of libdatachannel,
            // the mid() is used to associate candidates with media streams.
            rtc::Candidate c(candidate, sdpMid);
            sess->pc->addRemoteCandidate(c);
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[WebRtcReceiver] addIceCandidate failed: " << e.what() << std::endl;
            return false;
        }
    }

    void setVideoFrameCallback(WebRtcVideoFrameCallback cb) override {
        std::lock_guard<std::mutex> lock(mutex_);
        videoCb_ = std::move(cb);
    }

    void setAudioFrameCallback(WebRtcAudioFrameCallback cb) override {
        std::lock_guard<std::mutex> lock(mutex_);
        audioCb_ = std::move(cb);
    }

    void setSessionStateCallback(SessionStateCallback cb) override {
        stateCb_ = std::move(cb);
    }

    void setIceCandidateCallback(IceCandidateCallback cb) override {
        iceCb_ = std::move(cb);
    }

    std::optional<std::string> getLocalOffer(const std::string& /*sessionId*/) override {
        // We never offer.
        return std::nullopt;
    }

    std::optional<std::string> getLocalAnswer(const std::string& sessionId) override {
        std::shared_ptr<PerSession> sess;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(sessionId);
            if (it == sessions_.end()) return std::nullopt;
            sess = it->second;
        }
        std::lock_guard<std::mutex> lk(sess->mutex);
        if (!sess->haveLocalAnswer) {
            std::cerr << "[WebRtcReceiver] getLocalAnswer: no answer ready for " << sessionId << std::endl;
            return std::nullopt;
        }
        std::cout << "[WebRtcReceiver] " << sessionId << " getLocalAnswer: sdpBytes=" << sess->localAnswerSdp.size() << std::endl;
        return sess->localAnswerSdp;
    }

    std::string getSessionState(const std::string& sessionId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return "unknown";
        return toString(it->second->pcState.load());
    }

    bool setVideoEnabled(const std::string& sessionId, bool enabled) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return false;
        it->second->videoEnabled.store(enabled);
        return true;
    }

    bool setAudioEnabled(const std::string& sessionId, bool enabled) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return false;
        it->second->audioEnabled.store(enabled);
        return true;
    }

    bool hasSession(const std::string& sessionId) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_.find(sessionId) != sessions_.end();
    }

    std::vector<std::string> getAllSessionIds() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> ids;
        ids.reserve(sessions_.size());
        for (const auto& kv : sessions_) ids.push_back(kv.first);
        return ids;
    }

    void shutdown() override {
        if (!initialized_.exchange(false)) return;
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ids.reserve(sessions_.size());
            for (const auto& kv : sessions_) ids.push_back(kv.first);
        }
        for (auto& id : ids) removeSession(id);
        std::cout << "[WebRtcReceiver] shutdown complete" << std::endl;
    }

private:
    void notifyState(const std::string& sid, const std::string& state) {
        if (stateCb_) stateCb_(sid, state);
    }

    std::atomic<bool> initialized_{false};
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<PerSession>> sessions_;

    WebRtcVideoFrameCallback videoCb_;
    WebRtcAudioFrameCallback audioCb_;
    SessionStateCallback stateCb_;
    IceCandidateCallback iceCb_;
};

std::unique_ptr<WebRtcReceiver> createWebRtcReceiver() {
    return std::make_unique<WebRtcReceiverImpl>();
}

} // namespace phonecam
