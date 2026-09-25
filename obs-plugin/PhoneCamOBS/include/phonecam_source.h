#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>
#include <obs-module.h>
#include <obs-source.h>
#include <util/threading.h>
#include "ipc_client.h"

namespace phonecam_obs {

// OBS source private data
struct PhoneCamSourceData {
    // OBS source handle
    obs_source_t* source = nullptr;
    
    // Session configuration
    std::string sessionId;
    std::string deviceId;
    std::string pipeName;
    std::string token;
    
    // IPC client
    std::unique_ptr<IpcClient> ipcClient;
    
    // Video properties
    uint32_t videoWidth = 1920;
    uint32_t videoHeight = 1080;
    uint32_t videoFps = 30;
    
    // Audio properties
    uint32_t audioSampleRate = 48000;
    uint32_t audioChannels = 2;
    
    // State
    std::atomic<bool> active{false};
    std::atomic<bool> videoEnabled{true};
    std::atomic<bool> audioEnabled{true};
    std::atomic<bool> muted{false};
    std::atomic<float> gain{1.0f};
    
    // Threading
    std::thread videoThread;
    std::thread audioThread;
    std::mutex frameMutex;
    
    // Latest frames
    std::vector<uint8_t> latestVideoFrame;
    std::vector<float> latestAudioFrame;
    uint64_t videoTimestamp = 0;
    uint64_t audioTimestamp = 0;
    uint32_t videoFrameWidth = 0;
    uint32_t videoFrameHeight = 0;
    std::string videoFrameFormat;
    
    // Frame available flags
    std::atomic<bool> newVideoFrame{false};
    std::atomic<bool> newAudioFrame{false};
    
    // Video frame callback for OBS
    std::function<void(const uint8_t* data, size_t size, uint32_t width, uint32_t height, 
                       const char* format, uint64_t timestamp)> videoCallback;
    
    // Audio frame callback for OBS
    std::function<void(const float* data, size_t frames, uint32_t channels, 
                       uint32_t sampleRate, uint64_t timestamp)> audioCallback;
};

// Forward declarations for OBS callbacks
const char* phonecam_source_get_name(void* unused);
void phonecam_source_get_defaults(obs_data_t* settings);
obs_properties_t* phonecam_source_get_properties(void* unused);
void* phonecam_source_create(obs_data_t* settings, obs_source_t* source);
void phonecam_source_destroy(void* data);
void phonecam_source_update(void* data, obs_data_t* settings);
void phonecam_source_video_render(void* data, gs_effect_t* effect);
void phonecam_source_video_tick(void* data, float seconds);
uint32_t phonecam_source_get_width(void* data);
uint32_t phonecam_source_get_height(void* data);
void phonecam_source_audio_render(void* data, uint64_t* ts_out, struct obs_source_audio_mix* audio_output, uint32_t mixers, size_t channels, size_t sample_rate);
bool phonecam_source_audio_mixers(void* data);

} // namespace phonecam_obs

// OBS source info structure
extern struct obs_source_info phonecam_source_info;
