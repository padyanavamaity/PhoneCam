#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <utility>

// Include SessionManager for shared AudioFrame definition
#include "SessionManager.h"

namespace phonecam {

// Per-session audio configuration
struct AudioSessionConfig {
    std::string sessionId;
    std::string deviceId;
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    float gain = 1.0f;
    bool muted = false;
    int delayMs = 0; // Additional delay in milliseconds
    bool enabled = true;
};

// Consumer callback for real-time audio delivery
using AudioConsumerCallback = std::function<void(const std::string& sessionId, const AudioFrame& frame)>;

// Ring buffer for lock-free audio frame storage
template<typename T>
class LockFreeRingBuffer {
public:
    explicit LockFreeRingBuffer(size_t capacity) 
        : capacity_(capacity)
        , buffer_(std::make_unique<T[]>(capacity))
        , head_(0)
        , tail_(0)
        , count_(0) {}

    ~LockFreeRingBuffer() = default;

    // Non-copyable and non-movable: std::atomic members and the const
    // capacity make the previously defaulted move operations implicitly
    // deleted anyway. Deleting them explicitly documents that ring buffer
    // state must never be relocated while producers/consumers reference it.
    LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer(LockFreeRingBuffer&&) = delete;
    LockFreeRingBuffer& operator=(LockFreeRingBuffer&&) = delete;

    // Try to push a frame (non-blocking)
    bool tryPush(T&& value) {
        size_t currentTail = tail_.load(std::memory_order_relaxed);
        size_t nextTail = (currentTail + 1) % capacity_;
        
        if (nextTail == head_.load(std::memory_order_acquire)) {
            return false; // Buffer full
        }
        
        buffer_[currentTail] = std::move(value);
        tail_.store(nextTail, std::memory_order_release);
        count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Try to pop a frame (non-blocking)
    bool tryPop(T& outValue) {
        size_t currentHead = head_.load(std::memory_order_relaxed);
        
        if (currentHead == tail_.load(std::memory_order_acquire)) {
            return false; // Buffer empty
        }
        
        outValue = std::move(buffer_[currentHead]);
        head_.store((currentHead + 1) % capacity_, std::memory_order_release);
        count_.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }

    // Get current count (approximate)
    size_t size() const {
        return count_.load(std::memory_order_relaxed);
    }

    // Check if empty
    bool empty() const {
        return size() == 0;
    }

    // Check if full
    bool full() const {
        return size() >= capacity_ - 1; // Leave one slot empty for distinction
    }

    // Capacity
    size_t capacity() const {
        return capacity_;
    }

    // Clear buffer
    void clear() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
    }

private:
    const size_t capacity_;
    std::unique_ptr<T[]> buffer_;
    std::atomic<size_t> head_;
    std::atomic<size_t> tail_;
    std::atomic<size_t> count_;
};

// AudioDistributor class
class AudioDistributor {
public:
    AudioDistributor();
    ~AudioDistributor();

    // Non-copyable and non-movable: std::mutex/std::thread members make the
    // previously defaulted move operations implicitly deleted. Deleting them
    // explicitly documents that instances have a stable address (the consumer
    // thread captures `this`).
    AudioDistributor(const AudioDistributor&) = delete;
    AudioDistributor& operator=(const AudioDistributor&) = delete;
    AudioDistributor(AudioDistributor&&) = delete;
    AudioDistributor& operator=(AudioDistributor&&) = delete;

    // Initialize the distributor
    bool initialize();

    // Session management
    bool addSession(const AudioSessionConfig& config);
    bool removeSession(const std::string& sessionId);
    bool hasSession(const std::string& sessionId) const;
    std::vector<std::string> getAllSessionIds() const;

    // Session configuration
    bool setGain(const std::string& sessionId, float gain);
    bool setMuted(const std::string& sessionId, bool muted);
    bool setDelay(const std::string& sessionId, int delayMs);
    bool setEnabled(const std::string& sessionId, bool enabled);
    
    std::optional<AudioSessionConfig> getSessionConfig(const std::string& sessionId) const;

    // Push audio frame from WebRTC receiver (non-blocking)
    bool pushFrame(const std::string& sessionId, AudioFrame&& frame);

    // Consumer registration (for OBS, monitoring, etc.)
    //
    // Multiple consumers may be registered; frames are fanned out to all of
    // them. Consumer callbacks are invoked WITHOUT internal locks held, so
    // slow consumers (IPC, OBS) cannot stall the audio pipeline and may
    // safely call back into this object. Null callbacks are ignored.
    // unregisterConsumer() removes ALL registered consumers.
    void registerConsumer(AudioConsumerCallback consumer);
    void unregisterConsumer();

    // Get stats
    struct Stats {
        size_t totalFramesPushed = 0;
        size_t totalFramesDropped = 0;
        size_t totalFramesDelivered = 0;
        size_t activeSessions = 0;
        size_t registeredConsumers = 0;
    };
    Stats getStats() const;

    // Shutdown
    void shutdown();

private:
    struct SessionData {
        AudioSessionConfig config;
        LockFreeRingBuffer<AudioFrame> buffer;
        std::atomic<bool> active{true};
        
        // Fixed-size circular delay line for audio delay compensation.
        // (Re)sized lazily on the first frame after a delay change.
        // delayFilled tracks priming so a fresh line emits silence instead
        // of replaying stale audio from before the change.
        // Only touched while sessionsMutex_ is held (producer path).
        std::vector<AudioFrame> delayLine;
        size_t delayWrite = 0;
        size_t delayFilled = 0;
        
        explicit SessionData(size_t bufferCapacity) : buffer(bufferCapacity) {}
    };

    // Process frame with session config (gain, mute, delay).
    // Takes the frame by value so callers can move it in; no extra copies.
    AudioFrame processFrame(AudioFrame frame, SessionData& sessionData);

    // Deliver frame to consumer
    void deliverToConsumer(const std::string& sessionId, const AudioFrame& frame);

    mutable std::mutex sessionsMutex_;
    std::unordered_map<std::string, std::shared_ptr<SessionData>> sessions_;
    
    // Registered consumers (fan-out). Callbacks are snapshotted under the
    // mutex and invoked after it is released.
    std::mutex consumerMutex_;
    std::vector<AudioConsumerCallback> consumers_;
    
    std::atomic<bool> initialized_{false};
    std::atomic<bool> shutdown_{false};
    
    // Consumer thread for processing ring buffers
    std::thread consumerThread_;
    std::atomic<bool> consumerRunning_{false};
    
    // Work signalling for the consumer thread (replaces 1ms polling).
    // pendingFrames_ is the wait predicate; producers briefly lock
    // workMutex_ before notify so a notification can never be lost between
    // the consumer's predicate check and its block point.
    std::mutex workMutex_;
    std::condition_variable workCv_;
    std::atomic<size_t> pendingFrames_{0};
    
    void consumerLoop();
    
    // Stats
    mutable std::mutex statsMutex_;
    Stats stats_;
    
    static constexpr size_t DEFAULT_RING_BUFFER_SIZE = 480; // 10 seconds at 48kHz/20ms frames
    static constexpr size_t MAX_DELAY_FRAMES = 500;         // 5 seconds of 10ms frames
};

} // namespace phonecam