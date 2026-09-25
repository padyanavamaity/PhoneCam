#include "ipc_client.h"

#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#endif

#include <iostream>
#include <sstream>
#include <chrono>

namespace phonecam_obs {

// Helper: Base64 decode using OpenSSL
static std::vector<uint8_t> base64DecodeImpl(const std::string& data) {
    if (data.empty()) return {};
    
    BIO* bio = BIO_new_mem_buf(data.data(), static_cast<int>(data.size()));
    BIO* b64 = BIO_new(BIO_f_base64());
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    
    std::vector<uint8_t> buffer(data.size());
    int len = BIO_read(bio, buffer.data(), static_cast<int>(buffer.size()));
    
    BIO_free_all(bio);
    
    if (len <= 0) return {};
    buffer.resize(len);
    return buffer;
}

IpcClient::IpcClient(const IpcClientConfig& config)
    : config_(config) {
}

IpcClient::~IpcClient() {
    disconnect();
}

IpcClient::IpcClient(IpcClient&& other) noexcept
    : config_(std::move(other.config_))
    , pipeHandle_(other.pipeHandle_)
    , currentSessionId_(std::move(other.currentSessionId_))
    , currentToken_(std::move(other.currentToken_))
    , requestCounter_(other.requestCounter_.load())
    , lastError_(std::move(other.lastError_))
    , connected_(other.connected_.load()) {
    other.pipeHandle_ = INVALID_PIPE_HANDLE;
    other.connected_ = false;
}

IpcClient& IpcClient::operator=(IpcClient&& other) noexcept {
    if (this != &other) {
        disconnect();
        config_ = std::move(other.config_);
        pipeHandle_ = other.pipeHandle_;
        currentSessionId_ = std::move(other.currentSessionId_);
        currentToken_ = std::move(other.currentToken_);
        requestCounter_ = other.requestCounter_.load();
        lastError_ = std::move(other.lastError_);
        connected_ = other.connected_.load();
        other.pipeHandle_ = INVALID_PIPE_HANDLE;
        other.connected_ = false;
    }
    return *this;
}

bool IpcClient::connect(const std::string& pipeName) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (connected_) {
        lastError_ = "Already connected";
        return false;
    }
    
    if (!connectPlatform(pipeName)) {
        return false;
    }
    
    connected_ = true;
    lastError_.clear();
    return true;
}

void IpcClient::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!connected_) {
        return;
    }
    
    disconnectPlatform();
    pipeHandle_ = INVALID_PIPE_HANDLE;
    connected_ = false;
    currentSessionId_.clear();
    currentToken_.clear();
}

bool IpcClient::isConnected() const {
    return connected_.load();
}

void IpcClient::setSession(const std::string& sessionId, const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentSessionId_ = sessionId;
    currentToken_ = token;
}

std::optional<IpcResponse> IpcClient::sendRequest(IpcCommand command, const json& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!connected_) {
        lastError_ = "Not connected";
        return std::nullopt;
    }
    
    if (currentSessionId_.empty() || currentToken_.empty()) {
        lastError_ = "Session not set";
        return std::nullopt;
    }
    
    // Build request
    IpcRequest request;
    request.id = ++requestCounter_;
    request.command = command;
    request.sessionId = currentSessionId_;
    request.token = currentToken_;
    request.params = params;
    
    // Serialize
    auto requestData = serializeRequest(request);
    
    // Send
    if (!writePlatform(requestData)) {
        lastError_ = "Failed to write request";
        connected_ = false;
        return std::nullopt;
    }
    
    // Read response header (4 bytes length)
    uint32_t responseLength = 0;
    std::vector<uint8_t> header(4);
    if (!readPlatform(header, 4)) {
        lastError_ = "Failed to read response header";
        connected_ = false;
        return std::nullopt;
    }
    
    std::memcpy(&responseLength, header.data(), 4);
    
    // Validate length
    if (responseLength > config_.maxMessageSize || responseLength == 0) {
        lastError_ = "Invalid response length: " + std::to_string(responseLength);
        return std::nullopt;
    }
    
    // Read response body
    std::vector<uint8_t> responseData(responseLength);
    if (!readPlatform(responseData, responseLength)) {
        lastError_ = "Failed to read response body";
        connected_ = false;
        return std::nullopt;
    }
    
    // Parse response
    return deserializeResponse(responseData);
}

std::optional<VideoFrame> IpcClient::getVideoFrame() {
    auto response = sendRequest(IpcCommand::GET_VIDEO_FRAME);
    if (!response || response->status != IpcStatus::STATUS_OK) {
        if (response) {
            lastError_ = response->errorMessage;
        }
        return std::nullopt;
    }
    return parseVideoFrame(response->data);
}

std::optional<AudioFrame> IpcClient::getAudioFrame() {
    auto response = sendRequest(IpcCommand::GET_AUDIO_FRAME);
    if (!response || response->status != IpcStatus::STATUS_OK) {
        if (response) {
            lastError_ = response->errorMessage;
        }
        return std::nullopt;
    }
    return parseAudioFrame(response->data);
}

std::optional<SessionInfo> IpcClient::getSessionInfo() {
    auto response = sendRequest(IpcCommand::GET_SESSION_INFO);
    if (!response || response->status != IpcStatus::STATUS_OK) {
        if (response) {
            lastError_ = response->errorMessage;
        }
        return std::nullopt;
    }
    return parseSessionInfo(response->data);
}

bool IpcClient::subscribe() {
    auto response = sendRequest(IpcCommand::SUBSCRIBE);
    if (!response || response->status != IpcStatus::STATUS_OK) {
        if (response) {
            lastError_ = response->errorMessage;
        }
        return false;
    }
    return true;
}

bool IpcClient::unsubscribe() {
    auto response = sendRequest(IpcCommand::UNSUBSCRIBE);
    if (!response || response->status != IpcStatus::STATUS_OK) {
        if (response) {
            lastError_ = response->errorMessage;
        }
        return false;
    }
    return true;
}

std::string IpcClient::getLastError() const {
    return lastError_;
}

bool IpcClient::connectPlatform(const std::string& pipeName) {
#ifdef _WIN32
    // Convert to wide string
    int len = MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(), -1, nullptr, 0);
    std::wstring wpipeName(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(), -1, &wpipeName[0], len);
    
    // Wait for pipe to become available
    DWORD startTime = GetTickCount();
    while (GetTickCount() - startTime < config_.connectTimeoutMs) {
        pipeHandle_ = CreateFileW(
            wpipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr
        );
        
        if (pipeHandle_ != INVALID_HANDLE_VALUE) {
            break;
        }
        
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
            lastError_ = "CreateFileW failed: " + std::to_string(err);
            return false;
        }
        
        // Wait a bit and retry
        if (err == ERROR_PIPE_BUSY) {
            if (!WaitNamedPipeW(wpipeName.c_str(), 100)) {
                continue;
            }
        }
        
        Sleep(10);
    }
    
    if (pipeHandle_ == INVALID_HANDLE_VALUE) {
        lastError_ = "Connection timeout";
        return false;
    }
    
    // Set pipe to message mode
    DWORD mode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(pipeHandle_, &mode, nullptr, nullptr)) {
        lastError_ = "SetNamedPipeHandleState failed: " + std::to_string(GetLastError());
        CloseHandle(pipeHandle_);
        pipeHandle_ = INVALID_HANDLE_VALUE;
        return false;
    }
    
#else
    // Linux/Unix - use FIFO or Unix domain socket
    // For now, implement as FIFO
    pipeHandle_ = open(pipeName.c_str(), O_RDWR | O_NONBLOCK);
    if (pipeHandle_ < 0) {
        lastError_ = "open failed: " + std::string(strerror(errno));
        return false;
    }
    
    // Clear non-blocking
    int flags = fcntl(pipeHandle_, F_GETFL, 0);
    fcntl(pipeHandle_, F_SETFL, flags & ~O_NONBLOCK);
#endif
    
    return true;
}

void IpcClient::disconnectPlatform() {
#ifdef _WIN32
    if (pipeHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipeHandle_);
    }
#else
    if (pipeHandle_ >= 0) {
        close(pipeHandle_);
    }
#endif
}

bool IpcClient::writePlatform(const std::vector<uint8_t>& data) {
#ifdef _WIN32
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    
    DWORD totalWritten = 0;
    while (totalWritten < data.size()) {
        DWORD bytesWritten = 0;
        BOOL result = WriteFile(
            pipeHandle_,
            data.data() + totalWritten,
            static_cast<DWORD>(data.size() - totalWritten),
            &bytesWritten,
            &overlapped
        );
        
        if (!result) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD waitResult = WaitForSingleObject(overlapped.hEvent, config_.writeTimeoutMs);
                if (waitResult != WAIT_OBJECT_0) {
                    CloseHandle(overlapped.hEvent);
                    return false;
                }
                if (!GetOverlappedResult(pipeHandle_, &overlapped, &bytesWritten, FALSE)) {
                    CloseHandle(overlapped.hEvent);
                    return false;
                }
            } else {
                CloseHandle(overlapped.hEvent);
                return false;
            }
        }
        
        if (bytesWritten == 0) {
            CloseHandle(overlapped.hEvent);
            return false;
        }
        
        totalWritten += bytesWritten;
    }
    
    CloseHandle(overlapped.hEvent);
    return totalWritten == data.size();
#else
    ssize_t totalWritten = 0;
    while (totalWritten < static_cast<ssize_t>(data.size())) {
        ssize_t written = write(pipeHandle_, data.data() + totalWritten, data.size() - totalWritten);
        if (written < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!waitForWritable(config_.writeTimeoutMs)) return false;
                continue;
            }
            return false;
        }
        if (written == 0) return false;
        totalWritten += written;
    }
    return true;
#endif
}

bool IpcClient::readPlatform(std::vector<uint8_t>& outData, uint32_t expectedSize) {
#ifdef _WIN32
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    
    DWORD totalRead = 0;
    while (totalRead < expectedSize) {
        DWORD bytesRead = 0;
        BOOL result = ReadFile(
            pipeHandle_,
            outData.data() + totalRead,
            expectedSize - totalRead,
            &bytesRead,
            &overlapped
        );
        
        if (!result) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD waitResult = WaitForSingleObject(overlapped.hEvent, config_.readTimeoutMs);
                if (waitResult != WAIT_OBJECT_0) {
                    CloseHandle(overlapped.hEvent);
                    return false;
                }
                if (!GetOverlappedResult(pipeHandle_, &overlapped, &bytesRead, FALSE)) {
                    CloseHandle(overlapped.hEvent);
                    return false;
                }
            } else if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
                CloseHandle(overlapped.hEvent);
                return false;
            } else {
                CloseHandle(overlapped.hEvent);
                return false;
            }
        }
        
        if (bytesRead == 0) {
            CloseHandle(overlapped.hEvent);
            return false;
        }
        
        totalRead += bytesRead;
    }
    
    CloseHandle(overlapped.hEvent);
    return totalRead == expectedSize;
#else
    ssize_t totalRead = 0;
    while (totalRead < static_cast<ssize_t>(expectedSize)) {
        ssize_t bytesRead = read(pipeHandle_, outData.data() + totalRead, expectedSize - totalRead);
        if (bytesRead < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!waitForReadable(config_.readTimeoutMs)) return false;
                continue;
            }
            return false;
        }
        if (bytesRead == 0) return false;
        totalRead += bytesRead;
    }
    return true;
#endif
}

bool IpcClient::waitForReadable(uint32_t timeoutMs) {
#ifdef _WIN32
    (void)timeoutMs; // timeout parameter not used on Windows; readPlatform handles timing via overlapped I/O
    // On Windows, we use the overlapped event which is already handled in readPlatform
    return true;
#else
    struct pollfd pfd;
    pfd.fd = pipeHandle_;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, static_cast<int>(timeoutMs));
    return ret > 0 && (pfd.revents & POLLIN);
#endif
}

bool IpcClient::waitForWritable(uint32_t timeoutMs) {
#ifdef _WIN32
    (void)timeoutMs; // timeout parameter not used on Windows; writePlatform handles timing via overlapped I/O
    return true;
#else
    struct pollfd pfd;
    pfd.fd = pipeHandle_;
    pfd.events = POLLOUT;
    int ret = poll(&pfd, 1, static_cast<int>(timeoutMs));
    return ret > 0 && (pfd.revents & POLLOUT);
#endif
}

std::vector<uint8_t> IpcClient::serializeRequest(const IpcRequest& request) {
    json j;
    j["id"] = request.id;
    j["cmd"] = static_cast<uint32_t>(request.command);
    j["sessionId"] = request.sessionId;
    j["token"] = request.token;
    if (!request.params.empty()) {
        j["params"] = request.params;
    }
    
    std::string jsonStr = j.dump();
    return std::vector<uint8_t>(jsonStr.begin(), jsonStr.end());
}

std::optional<IpcResponse> IpcClient::deserializeResponse(const std::vector<uint8_t>& data) {
    try {
        std::string jsonStr(data.begin(), data.end());
        json j = json::parse(jsonStr);
        
        IpcResponse response;
        response.id = j.value("id", 0);
        response.status = static_cast<IpcStatus>(j.value("status", static_cast<uint32_t>(IpcStatus::STATUS_INTERNAL)));
        
        if (j.contains("data")) {
            response.data = j["data"];
        }
        if (j.contains("error")) {
            response.errorMessage = j["error"];
        }
        
        return response;
    } catch (const std::exception& e) {
        lastError_ = "JSON parse error: " + std::string(e.what());
        return std::nullopt;
    }
}

std::optional<VideoFrame> IpcClient::parseVideoFrame(const json& data) {
    try {
        VideoFrame frame;
        frame.width = data.value("width", 0);
        frame.height = data.value("height", 0);
        frame.format = data.value("format", "");
        frame.timestampUs = data.value("timestampUs", 0ULL);
        
        std::string base64Data = data.value("frameData", "");
        frame.data = base64Decode(base64Data);
        
        return frame;
    } catch (const std::exception& e) {
        lastError_ = "Video frame parse error: " + std::string(e.what());
        return std::nullopt;
    }
}

std::optional<AudioFrame> IpcClient::parseAudioFrame(const json& data) {
    try {
        AudioFrame frame;
        frame.sampleRate = data.value("sampleRate", 0);
        frame.channels = data.value("channels", 0);
        frame.samplesPerChannel = data.value("samplesPerChannel", 0);
        frame.format = data.value("format", "");
        frame.timestampUs = data.value("timestampUs", 0ULL);
        
        std::string base64Data = data.value("frameData", "");
        frame.data = base64DecodeFloat(base64Data);
        
        return frame;
    } catch (const std::exception& e) {
        lastError_ = "Audio frame parse error: " + std::string(e.what());
        return std::nullopt;
    }
}

std::optional<SessionInfo> IpcClient::parseSessionInfo(const json& data) {
    try {
        SessionInfo info;
        info.sessionId = data.value("sessionId", "");
        info.deviceId = data.value("deviceId", "");
        info.videoEnabled = data.value("videoEnabled", false);
        info.audioEnabled = data.value("audioEnabled", false);
        info.muted = data.value("muted", false);
        info.gain = data.value("gain", 1.0f);
        info.audioDelayMs = data.value("audioDelayMs", 0);
        info.width = data.value("width", 0);
        info.height = data.value("height", 0);
        info.fps = data.value("fps", 0);
        info.bitrateKbps = data.value("bitrateKbps", 0);
        info.codec = data.value("codec", "");
        info.connected = data.value("connected", false);
        
        return info;
    } catch (const std::exception& e) {
        lastError_ = "Session info parse error: " + std::string(e.what());
        return std::nullopt;
    }
}

std::vector<uint8_t> IpcClient::base64Decode(const std::string& data) {
    return base64DecodeImpl(data);
}

std::vector<float> IpcClient::base64DecodeFloat(const std::string& data) {
    auto bytes = base64DecodeImpl(data);
    if (bytes.size() % sizeof(float) != 0) {
        return {};
    }
    
    std::vector<float> result(bytes.size() / sizeof(float));
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

} // namespace phonecam_obs