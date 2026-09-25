#include "phonecam/AudioDistributor.h"

#include <iostream>
#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace phonecam {

AudioDistributor::AudioDistributor()
    : consumerThread_()
    , consumerRunning_(false) {
}

AudioDistributor::~AudioDistributor() {
    shutdown();
}

bool AudioDistributor::initialize() {
    if (initialized_.exchange(true)) {
        return true; // Already initialized
    }
    
    shutdown_.store(false);
    
    // Start consumer processing thread
    consumerRunning_.store(true);
    consumerThread_ = std::thread(&AudioDistributor::consumerLoop, this);
    
    std::cout << "[AudioDistributor] Initialized with consumer thread" << std::endl;
    return true;
}

bool AudioDistributor::addSession(const AudioSessionConfig& config) {
    if (!initialized_) {
        std::cerr << "[AudioDistributor] Not initialized" << std::endl;
        return false;
    }

    if (config.sessionId.empty() || config.deviceId.empty()) {
        std::cerr << "[AudioDistributor] Invalid session config" << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(sessionsMutex_);
    if (sessions_.find(config.sessionId) != sessions_.end()) {
        std::cerr << "[AudioDistributor] Session already exists: " << config.sessionId << std::endl;
        return false;
    }

    // Create session with ring buffer
    auto sessionData = std::make_shared<SessionData>(DEFAULT_RING_BUFFER_SIZE);
    sessionData->config = config;

    sessions_[config.sessionId] = std::move(sessionData);

    // Update stats
    {
        std::lock_guard<std::mutex> statsLock(statsMutex_);
        stats_.activeSessions = sessions_.size();
    }

    std::cout << "[AudioDistributor] Added session: " << config.sessionId << std::endl;
    return true;
}

bool AudioDistributor::removeSession(const std::string& sessionId) {
    if (sessionId.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return false;
    }

    it->second->active.store(false);
    sessions_.erase(it);

    // Update stats
    {
        std::lock_guard<std::mutex> statsLock(statsMutex_);
        stats_.activeSessions = sessions_.size();
    }

    std::cout << "[AudioDistributor] Removed session: " << sessionId << std::endl;
    return true;
}

bool AudioDistributor::hasSession(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    return sessions_.find(sessionId) != sessions_.end();
}

std::vector<std::string> AudioDistributor::getAllSessionIds() const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    std::vector<std::string> ids;
    ids.reserve(sessions_.size());
    for (const auto& [id, session] : sessions_) {
        ids.push_back(id);
    }
    return ids;
}

bool AudioDistributor::setGain(const std::string& sessionId, float gain) {
    if (gain < 0.0f || gain > 4.0f) {
        return false;
    }

    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return false;
    }
    it->second->config.gain = gain;
    return true;
}

bool AudioDistributor::setMuted(const std::string& sessionId, bool muted) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return false;
    }
    it->second->config.muted = muted;
    return true;
}

bool AudioDistributor::setDelay(const std::string& sessionId, int delayMs) {
    if (delayMs < 0 || delayMs > 5000) {
        return false;
    }

    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return false;
    }
    
    it->second->config.delayMs = delayMs;
    
    // Reset the delay line. It is (re)sized lazily inside processFrame so
    // the fill exactly matches the frame size actually in use, and so a
    // fresh line emits silence instead of replaying stale audio.
    it->second->delayLine.clear();
    it->second->delayWrite = 0;
    it->second->delayFilled = 0;
    
    return true;
}

bool AudioDistributor::setEnabled(const std::string& sessionId, bool enabled) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return false;
    }
    it->second->config.enabled = enabled;
    return true;
}

std::optional<AudioSessionConfig> AudioDistributor::getSessionConfig(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return std::nullopt;
    }
    return it->second->config;
}

bool AudioDistributor::pushFrame(const std::string& sessionId, AudioFrame&& frame) {
    if (shutdown_.load()) {
        return false;
    }

    bool pushed = false;
    {
        // Held across processing AND buffer push: processFrame touches the
        // per-session delay line, which setters also mutate under this lock.
        // Calling processFrame outside the lock would race with setDelay().
        std::lock_guard<std::mutex> lock(sessionsMutex_);

        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            // No session: frame dropped.
        } else {
            auto& sessionData = it->second;
            if (sessionData->active.load() && sessionData->config.enabled) {
                // processFrame takes the frame by value: exactly one move
                // happens here, zero copies in the hot path.
                AudioFrame processed = processFrame(std::move(frame), *sessionData);
                if (sessionData->buffer.tryPush(std::move(processed))) {
                    pushed = true;
                }
            }
        }
    }
    // Sessions lock released before updating stats or notifying.

    {
        std::lock_guard<std::mutex> statsLock(statsMutex_);
        if (pushed) {
            stats_.totalFramesPushed++;
        } else {
            stats_.totalFramesDropped++;
        }
    }

    if (pushed) {
        // Publish work before notifying so the consumer's predicate can
        // observe it. The brief workMutex_ lock guarantees a notification is
        // never lost between the consumer's predicate check and its block.
        pendingFrames_.fetch_add(1, std::memory_order_release);
        {
            std::lock_guard<std::mutex> workLock(workMutex_);
        }
        workCv_.notify_one();
    }

    return pushed;
}

AudioFrame AudioDistributor::processFrame(AudioFrame frame, SessionData& sessionData) {
    const auto& config = sessionData.config;
    
    // Apply mute (silence regardless of delay - mute is immediate)
    if (config.muted) {
        std::fill(frame.data.begin(), frame.data.end(), 0.0f);
        return frame;
    }
    
    // Apply gain with soft clipping
    if (config.gain != 1.0f) {
        for (float& sample : frame.data) {
            sample *= config.gain;
            if (sample > 1.0f) sample = 1.0f;
            else if (sample < -1.0f) sample = -1.0f;
        }
    }
    
    // Apply delay via fixed-size circular delay line, O(1) per frame.
    if (config.delayMs > 0) {
        // (Re)size lazily on first frame after a delay change.
        // The delay is expressed in frames of the actual size in use, never
        // exceeding the 5-second cap.
        if (sessionData.delayLine.empty()) {
            const size_t frameSamples = frame.data.size();
            if (frameSamples == 0) {
                return frame; // Empty frame; nothing meaningful to delay.
            }

            size_t delayFrames = 1;
            if (config.sampleRate > 0) {
                const size_t requestedSamples =
                    static_cast<size_t>(config.delayMs) * config.sampleRate / 1000;
                delayFrames = std::max<size_t>(1, (requestedSamples + frameSamples - 1) / frameSamples);
            }
            const size_t maxFrames = frameSamples > 0
                ? std::max<size_t>(1, (MAX_DELAY_FRAMES * 480) / frameSamples)
                : 1; // MAX_DELAY_FRAMES 10ms frames at 48kHz = 5 seconds
            delayFrames = std::min(delayFrames, maxFrames);

            sessionData.delayLine.resize(delayFrames);
            sessionData.delayWrite = 0;
            sessionData.delayFilled = 0;
        }

        // Slot to overwrite: the oldest frame once the line is primed.
        const size_t readSlot = sessionData.delayWrite;
        const bool primed = sessionData.delayFilled == sessionData.delayLine.size();

        AudioFrame& slot = sessionData.delayLine[readSlot];
        slot.data.swap(frame.data); // slot gets fresh audio, frame gets delayed audio
        slot.sampleRate = frame.sampleRate;
        slot.channels = frame.channels;

        sessionData.delayWrite = (readSlot + 1) % sessionData.delayLine.size();
        if (sessionData.delayFilled < sessionData.delayLine.size()) {
            sessionData.delayFilled++;
        }

        if (!primed) {
            // Delay line not full yet: emit silence of the correct duration,
            // keeping the current frame's metadata.
            std::fill(frame.data.begin(), frame.data.end(), 0.0f);
        }
        // Once primed, frame now holds the audio stored N frames ago - a
        // true delay, advancing every call with no element shifting.
    }
    
    return frame;
}

void AudioDistributor::registerConsumer(AudioConsumerCallback consumer) {
    if (!consumer) {
        return;
    }

    size_t consumerCount = 0;
    {
        std::lock_guard<std::mutex> lock(consumerMutex_);
        consumers_.push_back(std::move(consumer));
        consumerCount = consumers_.size();
    }
    
    {
        std::lock_guard<std::mutex> statsLock(statsMutex_);
        stats_.registeredConsumers = consumerCount;
    }
}

void AudioDistributor::unregisterConsumer() {
    {
        std::lock_guard<std::mutex> lock(consumerMutex_);
        consumers_.clear();
    }
    
    {
        std::lock_guard<std::mutex> statsLock(statsMutex_);
        stats_.registeredConsumers = 0;
    }
}

void AudioDistributor::deliverToConsumer(const std::string& sessionId, const AudioFrame& frame) {
    // Snapshot consumers under the lock, then release it before invoking
    // any callback. Consumer code (IPC, OBS) can be arbitrarily slow and
    // must never stall the consumer thread or deadlock by calling back
    // into register/unregister while the mutex is held.
    std::vector<AudioConsumerCallback> consumers;
    {
        std::lock_guard<std::mutex> lock(consumerMutex_);
        consumers = consumers_;
    }

    if (consumers.empty()) {
        return;
    }

    for (const auto& consumer : consumers) {
        if (consumer) {
            consumer(sessionId, frame);
        }
    }

    {
        std::lock_guard<std::mutex> statsLock(statsMutex_);
        stats_.totalFramesDelivered++;
    }
}

AudioDistributor::Stats AudioDistributor::getStats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void AudioDistributor::shutdown() {
    if (!initialized_.exchange(false)) {
        return; // Already shutdown
    }
    
    shutdown_.store(true);
    consumerRunning_.store(false);
    
    // Wake the consumer thread so it can observe the shutdown flags
    {
        std::lock_guard<std::mutex> workLock(workMutex_);
    }
    workCv_.notify_all();
    
    // Stop consumer thread
    if (consumerThread_.joinable()) {
        consumerThread_.join();
    }
    
    // Clear all sessions
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_.clear();
    }
    
    // Clear consumers
    {
        std::lock_guard<std::mutex> lock(consumerMutex_);
        consumers_.clear();
    }
    
    // Reset stats
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_ = Stats{};
    }

    pendingFrames_.store(0, std::memory_order_relaxed);
    
    std::cout << "[AudioDistributor] Shutdown complete" << std::endl;
}

// Consumer processing loop - pulls frames from ring buffers and delivers to consumers
void AudioDistributor::consumerLoop() {
    std::cout << "[AudioDistributor] Consumer thread started" << std::endl;
    
    while (consumerRunning_.load() && !shutdown_.load()) {
        bool anyFrameDelivered = false;

        // Snapshot session data pointers once per pass; shared_ptr keeps the
        // entries alive outside the mutex even if removeSession() erases them.
        std::vector<std::pair<std::string, std::shared_ptr<SessionData>>> snapshot;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            snapshot.reserve(sessions_.size());
            for (const auto& [id, session] : sessions_) {
                if (session->active.load() && session->config.enabled) {
                    snapshot.emplace_back(id, session);
                }
            }
        }

        // Process frames from each session (no sessionsMutex_ held here)
        for (const auto& [sessionId, sessionData] : snapshot) {
            AudioFrame frame;
            if (sessionData->buffer.tryPop(frame)) {
                deliverToConsumer(sessionId, frame);
                anyFrameDelivered = true;
                pendingFrames_.fetch_sub(1, std::memory_order_acq_rel);
            }
        }

        if (!anyFrameDelivered) {
            // Block until a producer publishes work (or shutdown) instead of
            // polling. Predicated wait makes the lost-wakeup race impossible.
            std::unique_lock<std::mutex> workLock(workMutex_);
            workCv_.wait(workLock, [this] {
                return pendingFrames_.load(std::memory_order_acquire) > 0
                    || shutdown_.load()
                    || !consumerRunning_.load();
            });
        }
    }
    
    std::cout << "[AudioDistributor] Consumer thread stopped" << std::endl;
}

} // namespace phonecam