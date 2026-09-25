#include "phonecam_source.h"

#include <obs-module.h>
#include <obs-source.h>
#include <util/threading.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <graphics/vec2.h>

#include <iostream>
#include <chrono>
#include <thread>
#include <cstring>

using namespace phonecam_obs;

namespace {

// Global plugin name
const char* g_pluginName = "PhoneCam OBS Source";

// Helper to convert OBS video format to string
const char* obs_video_format_to_string(enum video_format format) {
    switch (format) {
        case VIDEO_FORMAT_I420: return "I420";
        case VIDEO_FORMAT_NV12: return "NV12";
        case VIDEO_FORMAT_RGBA: return "RGBA";
        case VIDEO_FORMAT_BGRA: return "BGRA";
        case VIDEO_FORMAT_BGRX: return "BGRX";
        case VIDEO_FORMAT_Y800: return "Y800";
        case VIDEO_FORMAT_I422: return "I422";
        case VIDEO_FORMAT_I444: return "I444";
        case VIDEO_FORMAT_YVYU: return "YVYU";
        case VIDEO_FORMAT_YUY2: return "YUY2";
        case VIDEO_FORMAT_UYVY: return "UYVY";
        case VIDEO_FORMAT_NONE:
        default: return "UNKNOWN";
    }
}

// Helper to convert string to OBS video format
enum video_format string_to_obs_video_format(const char* str) {
    if (!str) return VIDEO_FORMAT_NONE;
    if (strcmp(str, "I420") == 0) return VIDEO_FORMAT_I420;
    if (strcmp(str, "NV12") == 0) return VIDEO_FORMAT_NV12;
    if (strcmp(str, "RGBA") == 0) return VIDEO_FORMAT_RGBA;
    if (strcmp(str, "BGRA") == 0) return VIDEO_FORMAT_BGRA;
    if (strcmp(str, "BGRX") == 0) return VIDEO_FORMAT_BGRX;
    if (strcmp(str, "Y800") == 0) return VIDEO_FORMAT_Y800;
    if (strcmp(str, "I422") == 0) return VIDEO_FORMAT_I422;
    if (strcmp(str, "I444") == 0) return VIDEO_FORMAT_I444;
    if (strcmp(str, "YVYU") == 0) return VIDEO_FORMAT_YVYU;
    if (strcmp(str, "YUY2") == 0) return VIDEO_FORMAT_YUY2;
    if (strcmp(str, "UYVY") == 0) return VIDEO_FORMAT_UYVY;
    return VIDEO_FORMAT_NONE;
}

} // anonymous namespace

// ===== OBS Source Callbacks =====

const char* phonecam_source_get_name(void* unused) {
    UNUSED_PARAMETER(unused);
    return "PhoneCam Source";
}

void phonecam_source_get_defaults(obs_data_t* settings) {
    obs_data_set_default_string(settings, "session_id", "");
    obs_data_set_default_string(settings, "device_id", "");
    obs_data_set_default_string(settings, "token", "");
    obs_data_set_default_bool(settings, "video_enabled", true);
    obs_data_set_default_bool(settings, "audio_enabled", true);
    obs_data_set_default_bool(settings, "muted", false);
    obs_data_set_default_double(settings, "gain", 1.0);
}

obs_properties_t* phonecam_source_get_properties(void* unused) {
    UNUSED_PARAMETER(unused);
    
    obs_properties_t* props = obs_properties_create();
    
    // Session ID (read-only, set via desktop app)
    obs_properties_add_text(props, "session_id", "Session ID", OBS_TEXT_DEFAULT);
    
    // Device ID (read-only)
    obs_properties_add_text(props, "device_id", "Device ID", OBS_TEXT_DEFAULT);
    
    // Token (read-only, set via desktop app)
    obs_properties_add_text(props, "token", "Auth Token", OBS_TEXT_PASSWORD);
    
    // Video enabled
    obs_properties_add_bool(props, "video_enabled", "Video Enabled");
    
    // Audio enabled
    obs_properties_add_bool(props, "audio_enabled", "Audio Enabled");
    
    // Muted
    obs_properties_add_bool(props, "muted", "Muted");
    
    // Gain
    obs_property_t* gainProp = obs_properties_add_float_slider(props, "gain", "Gain", 0.0, 10.0, 0.1);
    obs_property_set_modified_callback(gainProp, [](obs_properties_t* props, obs_property_t* p, obs_data_t* settings) {
        UNUSED_PARAMETER(props);
        UNUSED_PARAMETER(p);
        // Gain changes will be handled in update
    });
    
    // Refresh button to re-query session info
    obs_properties_add_button(props, "refresh", "Refresh Session Info", [](obs_properties_t* props, obs_property_t* p, void* data) {
        UNUSED_PARAMETER(props);
        UNUSED_PARAMETER(p);
        PhoneCamSourceData* sourceData = static_cast<PhoneCamSourceData*>(data);
        if (sourceData && sourceData->ipcClient && sourceData->ipcClient->isConnected()) {
            auto info = sourceData->ipcClient->getSessionInfo();
            if (info) {
                blog(LOG_INFO, "[PhoneCam] Session info: %s, %ux%u @%u fps, %s",
                     info->deviceId.c_str(), info->width, info->height, info->fps, info->codec.c_str());
            }
        }
        return true;
    }, nullptr);
    
    return props;
}

void* phonecam_source_create(obs_data_t* settings, obs_source_t* source) {
    auto* data = new PhoneCamSourceData();
    data->source = source;
    
    // Load settings
    data->sessionId = obs_data_get_string(settings, "session_id");
    data->deviceId = obs_data_get_string(settings, "device_id");
    data->token = obs_data_get_string(settings, "token");
    data->videoEnabled = obs_data_get_bool(settings, "video_enabled");
    data->audioEnabled = obs_data_get_bool(settings, "audio_enabled");
    data->muted = obs_data_get_bool(settings, "muted");
    data->gain = static_cast<float>(obs_data_get_double(settings, "gain"));
    
    // Construct pipe name
    if (!data->sessionId.empty()) {
        data->pipeName = "\\\\.\\pipe\\PhoneCam_" + data->sessionId;
    }
    
    // Initialize IPC client
    IpcClient::IpcClientConfig config;
    data->ipcClient = std::make_unique<IpcClient>(config);
    
    // Set up video callback
    data->videoCallback = [data](const uint8_t* frameData, size_t size, uint32_t width, uint32_t height,
                                  const char* format, uint64_t timestamp) {
        std::lock_guard<std::mutex> lock(data->frameMutex);
        
        data->latestVideoFrame.assign(frameData, frameData + size);
        data->videoFrameWidth = width;
        data->videoFrameHeight = height;
        data->videoFrameFormat = format ? format : "I420";
        data->videoTimestamp = timestamp;
        data->newVideoFrame = true;
    };
    
    // Set up audio callback
    data->audioCallback = [data](const float* frameData, size_t frames, uint32_t channels,
                                  uint32_t sampleRate, uint64_t timestamp) {
        std::lock_guard<std::mutex> lock(data->frameMutex);
        
        size_t sampleCount = frames * channels;
        data->latestAudioFrame.assign(frameData, frameData + sampleCount);
        data->audioTimestamp = timestamp;
        data->newAudioFrame = true;
    };
    
    // Connect to pipe if we have session info
    if (!data->pipeName.empty() && !data->token.empty()) {
        if (data->ipcClient->connect(data->pipeName)) {
            data->ipcClient->setSession(data->sessionId, data->token);
            data->ipcClient->subscribe();
            data->active = true;
            
            // Start video fetch thread
            data->videoThread = std::thread([data]() {
                while (data->active.load()) {
                    if (data->videoEnabled.load() && data->ipcClient && data->ipcClient->isConnected()) {
                        auto frame = data->ipcClient->getVideoFrame();
                        if (frame) {
                            data->videoCallback(frame->data.data(), frame->data.size(),
                                                frame->width, frame->height,
                                                frame->format.c_str(), frame->timestampUs);
                        }
                    }
                    // Sleep to maintain approximate frame rate
                    std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30fps
                }
            });
            
            // Start audio fetch thread
            data->audioThread = std::thread([data]() {
                while (data->active.load()) {
                    if (data->audioEnabled.load() && !data->muted.load() && data->ipcClient && data->ipcClient->isConnected()) {
                        auto frame = data->ipcClient->getAudioFrame();
                        if (frame) {
                            // Apply gain
                            if (data->gain.load() != 1.0f) {
                                for (auto& sample : frame->data) {
                                    sample *= data->gain.load();
                                }
                            }
                            data->audioCallback(frame->data.data(), frame->samplesPerChannel,
                                                frame->channels, frame->sampleRate, frame->timestampUs);
                        }
                    }
                    // Sleep for audio buffer duration (e.g., 10ms at 48kHz = 480 samples)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });
            
            blog(LOG_INFO, "[PhoneCam] Connected to session: %s", data->sessionId.c_str());
        } else {
            blog(LOG_WARNING, "[PhoneCam] Failed to connect to pipe: %s - %s",
                 data->pipeName.c_str(), data->ipcClient->getLastError().c_str());
        }
    }
    
    // Set OBS source info
    obs_source_set_output_flags(source, OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW);
    
    return data;
}

void phonecam_source_destroy(void* dataPtr) {
    PhoneCamSourceData* data = static_cast<PhoneCamSourceData*>(dataPtr);
    
    if (!data) return;
    
    // Stop threads
    data->active = false;
    if (data->videoThread.joinable()) {
        data->videoThread.join();
    }
    if (data->audioThread.joinable()) {
        data->audioThread.join();
    }
    
    // Disconnect IPC
    if (data->ipcClient) {
        data->ipcClient->unsubscribe();
        data->ipcClient->disconnect();
    }
    
    delete data;
}

void phonecam_source_update(void* dataPtr, obs_data_t* settings) {
    PhoneCamSourceData* data = static_cast<PhoneCamSourceData*>(dataPtr);
    
    if (!data) return;
    
    // Update settings
    std::string newSessionId = obs_data_get_string(settings, "session_id");
    std::string newDeviceId = obs_data_get_string(settings, "device_id");
    std::string newToken = obs_data_get_string(settings, "token");
    bool newVideoEnabled = obs_data_get_bool(settings, "video_enabled");
    bool newAudioEnabled = obs_data_get_bool(settings, "audio_enabled");
    bool newMuted = obs_data_get_bool(settings, "muted");
    float newGain = static_cast<float>(obs_data_get_double(settings, "gain"));
    
    // Check if session changed (need to reconnect)
    bool sessionChanged = (newSessionId != data->sessionId) || (newToken != data->token);
    
    data->sessionId = newSessionId;
    data->deviceId = newDeviceId;
    data->token = newToken;
    data->videoEnabled = newVideoEnabled;
    data->audioEnabled = newAudioEnabled;
    data->muted = newMuted;
    data->gain = newGain;
    
    // Update pipe name
    if (!data->sessionId.empty()) {
        data->pipeName = "\\\\.\\pipe\\PhoneCam_" + data->sessionId;
    }
    
    // Reconnect if session changed
    if (sessionChanged && !data->sessionId.empty() && !data->token.empty()) {
        // Stop old threads
        data->active = false;
        if (data->videoThread.joinable()) {
            data->videoThread.join();
        }
        if (data->audioThread.joinable()) {
            data->audioThread.join();
        }
        
        // Reconnect
        if (data->ipcClient) {
            data->ipcClient->disconnect();
        }
        
        data->ipcClient = std::make_unique<IpcClient>(IpcClient::IpcClientConfig());
        if (data->ipcClient->connect(data->pipeName)) {
            data->ipcClient->setSession(data->sessionId, data->token);
            data->ipcClient->subscribe();
            data->active = true;
            
            // Restart threads
            data->videoThread = std::thread([data]() {
                while (data->active.load()) {
                    if (data->videoEnabled.load() && data->ipcClient && data->ipcClient->isConnected()) {
                        auto frame = data->ipcClient->getVideoFrame();
                        if (frame) {
                            data->videoCallback(frame->data.data(), frame->data.size(),
                                                frame->width, frame->height,
                                                frame->format.c_str(), frame->timestampUs);
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(33));
                }
            });
            
            data->audioThread = std::thread([data]() {
                while (data->active.load()) {
                    if (data->audioEnabled.load() && !data->muted.load() && data->ipcClient && data->ipcClient->isConnected()) {
                        auto frame = data->ipcClient->getAudioFrame();
                        if (frame) {
                            if (data->gain.load() != 1.0f) {
                                for (auto& sample : frame->data) {
                                    sample *= data->gain.load();
                                }
                            }
                            data->audioCallback(frame->data.data(), frame->samplesPerChannel,
                                                frame->channels, frame->sampleRate, frame->timestampUs);
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });
            
            blog(LOG_INFO, "[PhoneCam] Reconnected to session: %s", data->sessionId.c_str());
        } else {
            blog(LOG_WARNING, "[PhoneCam] Failed to reconnect to pipe: %s - %s",
                 data->pipeName.c_str(), data->ipcClient->getLastError().c_str());
        }
    }
}

void phonecam_source_video_render(void* dataPtr, gs_effect_t* effect) {
    UNUSED_PARAMETER(effect);
    
    PhoneCamSourceData* data = static_cast<PhoneCamSourceData*>(dataPtr);
    
    if (!data || !data->videoEnabled.load() || !data->newVideoFrame.load()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(data->frameMutex);
    
    if (data->latestVideoFrame.empty()) {
        return;
    }
    
    // Create video frame for OBS
    struct obs_source_frame frame = {};
    frame.format = string_to_obs_video_format(data->videoFrameFormat.c_str());
    frame.width = data->videoFrameWidth;
    frame.height = data->videoFrameHeight;
    frame.timestamp = data->videoTimestamp;
    
    // For I420, we need to set up planes correctly
    if (frame.format == VIDEO_FORMAT_I420) {
        size_t ySize = frame.width * frame.height;
        size_t uvSize = ySize / 4;
        
        frame.data[0] = data->latestVideoFrame.data();
        frame.data[1] = data->latestVideoFrame.data() + ySize;
        frame.data[2] = data->latestVideoFrame.data() + ySize + uvSize;
        
        frame.linesize[0] = frame.width;
        frame.linesize[1] = frame.width / 2;
        frame.linesize[2] = frame.width / 2;
    } else if (frame.format == VIDEO_FORMAT_NV12) {
        size_t ySize = frame.width * frame.height;
        
        frame.data[0] = data->latestVideoFrame.data();
        frame.data[1] = data->latestVideoFrame.data() + ySize;
        
        frame.linesize[0] = frame.width;
        frame.linesize[1] = frame.width;
    } else {
        // Packed formats (RGBA, BGRA, etc.)
        frame.data[0] = data->latestVideoFrame.data();
        frame.linesize[0] = frame.width * 4; // Assuming 4 bytes per pixel
    }
    
    // Push frame to OBS
    obs_source_output_video(data->source, &frame);
    
    data->newVideoFrame = false;
}

void phonecam_source_video_tick(void* dataPtr, float seconds) {
    UNUSED_PARAMETER(seconds);
    
    PhoneCamSourceData* data = static_cast<PhoneCamSourceData*>(dataPtr);
    
    if (!data || !data->active.load()) {
        return;
    }
    
    // Video tick is called from OBS video thread
    // We use this to signal that we have a new frame
    // The actual frame fetching happens in our worker thread
}

uint32_t phonecam_source_get_width(void* dataPtr) {
    PhoneCamSourceData* data = static_cast<PhoneCamSourceData*>(dataPtr);
    if (!data) return 0;
    return data->videoFrameWidth > 0 ? data->videoFrameWidth : data->videoWidth;
}

uint32_t phonecam_source_get_height(void* dataPtr) {
    PhoneCamSourceData* data = static_cast<PhoneCamSourceData*>(dataPtr);
    if (!data) return 0;
    return data->videoFrameHeight > 0 ? data->videoFrameHeight : data->videoHeight;
}

void phonecam_source_audio_render(void* dataPtr, uint64_t* ts_out, struct obs_source_audio_mix* audio_output,
                                   uint32_t mixers, size_t channels, size_t sample_rate) {
    PhoneCamSourceData* data = static_cast<PhoneCamSourceData*>(dataPtr);
    
    if (!data || !data->audioEnabled.load() || data->muted.load() || !data->newAudioFrame.load()) {
        // Output silence
        for (uint32_t i = 0; i < channels; i++) {
            memset(audio_output->output[i].data, 0, audio_output->output[i].len * sizeof(float));
        }
        *ts_out = 0;
        return;
    }
    
    std::lock_guard<std::mutex> lock(data->frameMutex);
    
    if (data->latestAudioFrame.empty()) {
        // Output silence
        for (uint32_t i = 0; i < channels; i++) {
            memset(audio_output->output[i].data, 0, audio_output->output[i].len * sizeof(float));
        }
        *ts_out = 0;
        return;
    }
    
    *ts_out = data->audioTimestamp;
    
    // Copy audio data to OBS output buffers
    // OBS expects planar format, but we have interleaved
    size_t framesPerChannel = audio_output->output[0].len / sizeof(float);
    size_t sourceFrames = data->latestAudioFrame.size() / channels;
    size_t framesToCopy = std::min(framesPerChannel, sourceFrames);
    
    if (channels == 2 && data->latestAudioFrame.size() >= framesToCopy * 2) {
        // Stereo: deinterleave
        const float* src = data->latestAudioFrame.data();
        float* dstL = (float*)audio_output->output[0].data;
        float* dstR = (float*)audio_output->output[1].data;
        
        for (size_t i = 0; i < framesToCopy; i++) {
            dstL[i] = src[i * 2];
            dstR[i] = src[i * 2 + 1];
        }
        
        // Zero remaining
        if (framesToCopy < framesPerChannel) {
            memset(dstL + framesToCopy, 0, (framesPerChannel - framesToCopy) * sizeof(float));
            memset(dstR + framesToCopy, 0, (framesPerChannel - framesToCopy) * sizeof(float));
        }
    } else if (channels == 1) {
        // Mono
        float* dst = (float*)audio_output->output[0].data;
        memcpy(dst, data->latestAudioFrame.data(), framesToCopy * sizeof(float));
        if (framesToCopy < framesPerChannel) {
            memset(dst + framesToCopy, 0, (framesPerChannel - framesToCopy) * sizeof(float));
        }
    } else {
        // Multi-channel - copy what we can
        for (uint32_t ch = 0; ch < channels && ch < data->latestAudioFrame.size() / framesToCopy; ch++) {
            const float* src = data->latestAudioFrame.data() + ch;
            float* dst = (float*)audio_output->output[ch].data;
            
            for (size_t i = 0; i < framesToCopy; i++) {
                dst[i] = src[i * channels];
            }
        }
    }
    
    data->newAudioFrame = false;
}

bool phonecam_source_audio_mixers(void* dataPtr) {
    UNUSED_PARAMETER(dataPtr);
    return true; // Output to all mixers
}

// ===== OBS Source Info =====

struct obs_source_info phonecam_source_info = {
    .id = "phonecam_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW,
    .get_name = phonecam_source_get_name,
    .get_defaults = phonecam_source_get_defaults,
    .get_properties = phonecam_source_get_properties,
    .create = phonecam_source_create,
    .destroy = phonecam_source_destroy,
    .update = phonecam_source_update,
    .video_render = phonecam_source_video_render,
    .video_tick = phonecam_source_video_tick,
    .get_width = phonecam_source_get_width,
    .get_height = phonecam_source_get_height,
    .audio_render = phonecam_source_audio_render,
    .audio_mixers = phonecam_source_audio_mixers,
};

// ===== Plugin Entry Point =====

extern "C" {
    OBS_DECLARE_MODULE()
    OBS_MODULE_USE_DEFAULT_LOCALE("phonecam-obs", "en-US")
    
    MODULE_EXPORT const char* obs_module_description(void) {
        return "PhoneCam OBS Source Plugin - Professional multi-camera production";
    }
    
    MODULE_EXPORT bool obs_module_load(void) {
        blog(LOG_INFO, "[PhoneCam] Loading PhoneCam OBS plugin");
        
        obs_register_source(&phonecam_source_info);
        
        blog(LOG_INFO, "[PhoneCam] PhoneCam OBS plugin loaded successfully");
        return true;
    }
    
    MODULE_EXPORT void obs_module_unload(void) {
        blog(LOG_INFO, "[PhoneCam] Unloading PhoneCam OBS plugin");
    }
    
    // Keep the old plugin name function for compatibility
    MODULE_EXPORT const char* phonecam_obs_plugin_name() {
        return g_pluginName;
    }
}