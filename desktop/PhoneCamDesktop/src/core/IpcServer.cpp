#include "phonecam/IpcServer.h"
#include "phonecam/SecurityManager.h"

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#endif

#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

// OpenSSL for base64
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

namespace phonecam
{

    // Helper: Base64 encode using OpenSSL
    static std::string base64Encode(const std::vector<uint8_t> &data)
    {
        if (data.empty())
            return "";

        BIO *bio = BIO_new(BIO_f_base64());
        BIO *mem = BIO_new(BIO_s_mem());
        bio = BIO_push(bio, mem);
        BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(bio, data.data(), static_cast<int>(data.size()));
        BIO_flush(bio);

        BUF_MEM *bptr;
        BIO_get_mem_ptr(bio, &bptr);
        std::string result(bptr->data, bptr->length);

        BIO_free_all(bio);
        return result;
    }

    // Helper: Base64 encode float array
    static std::string base64EncodeFloat(const std::vector<float> &data)
    {
        if (data.empty())
            return "";

        std::vector<uint8_t> bytes(data.size() * sizeof(float));
        std::memcpy(bytes.data(), data.data(), bytes.size());
        return base64Encode(bytes);
    }

    IpcServer::IpcServer(
        SecurityManager *securityManager,
        SessionManager *sessionManager,
        const IpcServerConfig &config) : config_(config), securityManager_(securityManager), sessionManager_(sessionManager), running_(false)
    {
    }

    IpcServer::~IpcServer()
    {
        stop();
    }

    IpcServer::IpcServer(IpcServer &&other) noexcept
        : config_(std::move(other.config_)), securityManager_(other.securityManager_), sessionManager_(other.sessionManager_), running_(other.running_.load()), acceptorThread_(std::move(other.acceptorThread_)), pipes_(std::move(other.pipes_)), requestCounter_(other.requestCounter_.load()), videoPushCallback_(std::move(other.videoPushCallback_)), audioPushCallback_(std::move(other.audioPushCallback_))
    {
    }

    IpcServer &IpcServer::operator=(IpcServer &&other) noexcept
    {
        if (this != &other)
        {
            stop();
            config_ = std::move(other.config_);
            securityManager_ = other.securityManager_;
            sessionManager_ = other.sessionManager_;
            running_ = other.running_.load();
            acceptorThread_ = std::move(other.acceptorThread_);
            pipes_ = std::move(other.pipes_);
            requestCounter_ = other.requestCounter_.load();
            videoPushCallback_ = std::move(other.videoPushCallback_);
            audioPushCallback_ = std::move(other.audioPushCallback_);
        }
        return *this;
    }

    IpcServer::SecurityDescriptorPtr
    IpcServer::createSecurityDescriptor()
    {
        // ACL allowing:
        // - OBS process (we'll identify by executable path at runtime)
        // - LOCAL SYSTEM (S-1-5-18)
        // - Administrators (S-1-5-32-544)

        // Build SDDL string for the security descriptor
        // D: - Discretionary ACL
        // (A;;GA;;;SY) - Allow Generic All to Local System
        // (A;;GA;;;BA) - Allow Generic All to Built-in Administrators
        // (A;;GRGW;;;IU) - Allow Generic Read/Write to Interactive User (for OBS running as user)
        // Note: For tighter security, we could restrict to specific executable path

        const char *sddl = "D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)";

        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
                sddl, SDDL_REVISION_1, &sd, nullptr))
        {
            DWORD err = GetLastError();
            std::cerr << "[IpcServer] Failed to create security descriptor: " << err << std::endl;
            return SecurityDescriptorPtr(nullptr);
        }

        // The unique_ptr will use the SecurityDescriptorDeleter functor automatically
        // PSECURITY_DESCRIPTOR is SECURITY_DESCRIPTOR*, so we cast to the correct pointer type
        return SecurityDescriptorPtr(static_cast<SECURITY_DESCRIPTOR *>(sd));
    }

    bool IpcServer::start()
    {
        if (running_.exchange(true))
        {
            return true; // Already running
        }

        // The server creates pipes on-demand per session via createSessionPipe()
        // No single acceptor thread needed - each session gets its own pipe instance
        std::cout << "[IpcServer] Started" << std::endl;
        return true;
    }

    void IpcServer::stop()
    {
        if (!running_.exchange(false))
        {
            return; // Already stopped
        }

        // Close all pipe instances
        {
            std::lock_guard<std::mutex> lock(pipesMutex_);
            for (auto &[sessionId, pipe] : pipes_)
            {
                if (pipe->pipeHandle != INVALID_HANDLE_VALUE)
                {
                    CancelIoEx(pipe->pipeHandle, nullptr);
                    DisconnectNamedPipe(pipe->pipeHandle);
                    CloseHandle(pipe->pipeHandle);
                    pipe->pipeHandle = INVALID_HANDLE_VALUE;
                }
                pipe->active = false;
                if (pipe->workerThread.joinable())
                {
                    pipe->workerThread.join();
                }
            }
            pipes_.clear();
        }

        std::cout << "[IpcServer] Stopped" << std::endl;
    }

    bool IpcServer::isRunning() const
    {
        return running_.load();
    }

    std::string IpcServer::createSessionPipe(const std::string &sessionId)
    {
        std::string pipeName = R"(\\.\pipe\)" + config_.pipeNamePrefix + sessionId;

        auto sd = createSecurityDescriptor();
        if (!sd)
        {
            std::cerr << "[IpcServer] Failed to create security descriptor for session: " << sessionId << std::endl;
            return "";
        }

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.lpSecurityDescriptor = sd.get();
        sa.bInheritHandle = FALSE;

        HANDLE pipeHandle = CreateNamedPipeW(
            // Convert to wide string
            [&pipeName]() -> std::wstring
                             {
                                 int len = MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(), -1, nullptr, 0);
                                 std::wstring wstr(len, 0);
                                 MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(), -1, &wstr[0], len);
                                 return wstr;
                             }()
                                 .c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            config_.maxPendingConnections,
            config_.maxMessageSize,
            config_.maxMessageSize,
            config_.connectionTimeoutMs,
            &sa);

        if (pipeHandle == INVALID_HANDLE_VALUE)
        {
            DWORD err = GetLastError();
            std::cerr << "[IpcServer] CreateNamedPipeW failed for " << sessionId << ": " << err << std::endl;
            return "";
        }

        // Create pipe instance
        auto instance = std::make_unique<PipeInstance>();
        instance->pipeHandle = pipeHandle;
        instance->sessionId = sessionId;
        instance->readBuffer.resize(4); // Start with header (4 bytes for length)
        instance->readOverlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        instance->writeOverlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        instance->active = true;

        // Start worker thread for this pipe
        instance->workerThread = std::thread(&IpcServer::serverThreadFunc, this, sessionId);

        {
            std::lock_guard<std::mutex> lock(pipesMutex_);
            pipes_[sessionId] = std::move(instance);
        }

        std::cout << "[IpcServer] Created pipe for session: " << sessionId << " (" << pipeName << ")" << std::endl;
        return pipeName;
    }

    void IpcServer::removeSessionPipe(const std::string &sessionId)
    {
        std::unique_ptr<PipeInstance> instance;

        {
            std::lock_guard<std::mutex> lock(pipesMutex_);
            auto it = pipes_.find(sessionId);
            if (it != pipes_.end())
            {
                instance = std::move(it->second);
                pipes_.erase(it);
            }
        }

        if (instance)
        {
            instance->active = false;
            if (instance->pipeHandle != INVALID_HANDLE_VALUE)
            {
                CancelIoEx(instance->pipeHandle, nullptr);
                DisconnectNamedPipe(instance->pipeHandle);
                CloseHandle(instance->pipeHandle);
                instance->pipeHandle = INVALID_HANDLE_VALUE;
            }
            if (instance->readOverlapped.hEvent)
            {
                CloseHandle(instance->readOverlapped.hEvent);
                instance->readOverlapped.hEvent = nullptr;
            }
            if (instance->writeOverlapped.hEvent)
            {
                CloseHandle(instance->writeOverlapped.hEvent);
                instance->writeOverlapped.hEvent = nullptr;
            }
            if (instance->workerThread.joinable())
            {
                instance->workerThread.join();
            }
            std::cout << "[IpcServer] Removed pipe for session: " << sessionId << std::endl;
        }
    }

    size_t IpcServer::getActiveSessionCount() const
    {
        std::lock_guard<std::mutex> lock(pipesMutex_);
        return pipes_.size();
    }

    void IpcServer::serverThreadFunc(const std::string &sessionId)
    {
        std::unique_ptr<PipeInstance> instance;

        {
            std::lock_guard<std::mutex> lock(pipesMutex_);
            auto it = pipes_.find(sessionId);
            if (it != pipes_.end())
            {
                instance = std::move(it->second);
                pipes_.erase(it);
            }
        }

        if (!instance)
        {
            return;
        }

        // Wait for client connection
        BOOL connected = ConnectNamedPipe(instance->pipeHandle, &instance->readOverlapped);
        if (!connected)
        {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING)
            {
                // Wait for connection
                DWORD waitResult = WaitForSingleObject(instance->readOverlapped.hEvent, config_.connectionTimeoutMs);
                if (waitResult != WAIT_OBJECT_0)
                {
                    std::cerr << "[IpcServer] Connection timeout for session: " << sessionId << std::endl;
                    return;
                }
                DWORD bytesTransferred;
                if (!GetOverlappedResult(instance->pipeHandle, &instance->readOverlapped, &bytesTransferred, FALSE))
                {
                    std::cerr << "[IpcServer] ConnectNamedPipe failed for session: " << sessionId << std::endl;
                    return;
                }
            }
            else if (err != ERROR_PIPE_CONNECTED)
            {
                std::cerr << "[IpcServer] ConnectNamedPipe error for session " << sessionId << ": " << err << std::endl;
                return;
            }
        }

        ResetEvent(instance->readOverlapped.hEvent);

        std::cout << "[IpcServer] Client connected to session: " << sessionId << std::endl;

        // Handle client communication
        handleClientConnection(*instance);

        // Cleanup
        if (instance->pipeHandle != INVALID_HANDLE_VALUE)
        {
            DisconnectNamedPipe(instance->pipeHandle);
            CloseHandle(instance->pipeHandle);
            instance->pipeHandle = INVALID_HANDLE_VALUE;
        }
        if (instance->readOverlapped.hEvent)
        {
            CloseHandle(instance->readOverlapped.hEvent);
        }
        if (instance->writeOverlapped.hEvent)
        {
            CloseHandle(instance->writeOverlapped.hEvent);
        }

        std::cout << "[IpcServer] Client disconnected from session: " << sessionId << std::endl;
    }

    void IpcServer::handleClientConnection(PipeInstance &instance)
    {
        while (instance.active && running_)
        {
            std::vector<uint8_t> message;
            if (!readMessage(instance, message))
            {
                break; // Client disconnected or error
            }

            // Deserialize request
            auto request = deserializeRequest(message);
            if (!request)
            {
                // Send error response
                IpcResponse response;
                response.id = 0;
                response.status = IpcStatus::ERR_INVALID_REQUEST;
                response.errorMessage = "Failed to parse request";
                auto responseData = serializeResponse(response);
                writeMessage(instance, responseData);
                continue;
            }

            // Process request
            IpcResponse response = processRequest(*request);

            // Send response
            auto responseData = serializeResponse(response);
            if (!writeMessage(instance, responseData))
            {
                break; // Write failed
            }
        }
    }

    bool IpcServer::readMessage(PipeInstance &instance, std::vector<uint8_t> &outMessage)
    {
        // Read length prefix (4 bytes, little-endian)
        uint32_t messageLength = 0;
        DWORD totalRead = 0;

        while (totalRead < 4 && instance.active && running_)
        {
            DWORD bytesRead = 0;
            BOOL result = ReadFile(
                instance.pipeHandle,
                reinterpret_cast<char *>(&messageLength) + totalRead,
                4 - totalRead,
                &bytesRead,
                &instance.readOverlapped);

            if (!result)
            {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING)
                {
                    DWORD waitResult = WaitForSingleObject(instance.readOverlapped.hEvent, config_.readTimeoutMs);
                    if (waitResult != WAIT_OBJECT_0)
                    {
                        return false; // Timeout
                    }
                    if (!GetOverlappedResult(instance.pipeHandle, &instance.readOverlapped, &bytesRead, FALSE))
                    {
                        return false; // Error or disconnect
                    }
                }
                else if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED)
                {
                    return false; // Client disconnected
                }
                else
                {
                    return false; // Other error
                }
            }

            if (bytesRead == 0)
            {
                return false; // Disconnected
            }

            totalRead += bytesRead;
        }

        if (totalRead != 4)
        {
            return false;
        }

        // Validate message length
        if (messageLength > config_.maxMessageSize || messageLength == 0)
        {
            std::cerr << "[IpcServer] Invalid message length: " << messageLength << std::endl;
            return false;
        }

        // Read message payload
        outMessage.resize(messageLength);
        totalRead = 0;

        while (totalRead < messageLength && instance.active && running_)
        {
            DWORD bytesRead = 0;
            BOOL result = ReadFile(
                instance.pipeHandle,
                outMessage.data() + totalRead,
                messageLength - totalRead,
                &bytesRead,
                &instance.readOverlapped);

            if (!result)
            {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING)
                {
                    DWORD waitResult = WaitForSingleObject(instance.readOverlapped.hEvent, config_.readTimeoutMs);
                    if (waitResult != WAIT_OBJECT_0)
                    {
                        return false;
                    }
                    if (!GetOverlappedResult(instance.pipeHandle, &instance.readOverlapped, &bytesRead, FALSE))
                    {
                        return false;
                    }
                }
                else if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED)
                {
                    return false;
                }
                else
                {
                    return false;
                }
            }

            if (bytesRead == 0)
            {
                return false;
            }

            totalRead += bytesRead;
        }

        return totalRead == messageLength;
    }

    bool IpcServer::writeMessage(PipeInstance &instance, const std::vector<uint8_t> &message)
    {
        // Write length prefix (4 bytes, little-endian)
        uint32_t length = static_cast<uint32_t>(message.size());
        std::vector<uint8_t> header(4);
        std::memcpy(header.data(), &length, 4);

        // Combine header + message
        std::vector<uint8_t> fullMessage;
        fullMessage.reserve(4 + message.size());
        fullMessage.insert(fullMessage.end(), header.begin(), header.end());
        fullMessage.insert(fullMessage.end(), message.begin(), message.end());

        DWORD totalWritten = 0;
        while (totalWritten < fullMessage.size() && instance.active && running_)
        {
            DWORD bytesWritten = 0;
            BOOL result = WriteFile(
                instance.pipeHandle,
                fullMessage.data() + totalWritten,
                static_cast<DWORD>(fullMessage.size() - totalWritten),
                &bytesWritten,
                &instance.writeOverlapped);

            if (!result)
            {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING)
                {
                    DWORD waitResult = WaitForSingleObject(instance.writeOverlapped.hEvent, config_.writeTimeoutMs);
                    if (waitResult != WAIT_OBJECT_0)
                    {
                        return false;
                    }
                    if (!GetOverlappedResult(instance.pipeHandle, &instance.writeOverlapped, &bytesWritten, FALSE))
                    {
                        return false;
                    }
                }
                else
                {
                    return false;
                }
            }

            if (bytesWritten == 0)
            {
                return false;
            }

            totalWritten += bytesWritten;
        }

        return totalWritten == fullMessage.size();
    }

    IpcResponse IpcServer::processRequest(const IpcRequest &request)
    {
        IpcResponse response;
        response.id = request.id;

        // Validate token
        if (!validateToken(request.sessionId, request.token))
        {
            response.status = IpcStatus::ERR_INVALID_TOKEN;
            response.errorMessage = "Invalid or expired session token";
            return response;
        }

        // Route to handler
        switch (request.command)
        {
        case IpcCommand::GET_VIDEO_FRAME:
            return handleGetVideoFrame(request);
        case IpcCommand::GET_AUDIO_FRAME:
            return handleGetAudioFrame(request);
        case IpcCommand::GET_SESSION_INFO:
            return handleGetSessionInfo(request);
        case IpcCommand::SUBSCRIBE:
            return handleSubscribe(request);
        case IpcCommand::UNSUBSCRIBE:
            return handleUnsubscribe(request);
        default:
            response.status = IpcStatus::ERR_INVALID_REQUEST;
            response.errorMessage = "Unknown command";
            return response;
        }
    }

    IpcResponse IpcServer::handleGetVideoFrame(const IpcRequest &request)
    {
        IpcResponse response;
        response.id = request.id;

        if (!sessionManager_)
        {
            response.status = IpcStatus::ERR_INTERNAL;
            response.errorMessage = "Session manager not available";
            return response;
        }

        // Use SessionManager's frame retrieval which uses registered callbacks
        auto frameOpt = sessionManager_->getVideoFrame(request.sessionId);
        if (!frameOpt)
        {
            response.status = IpcStatus::ERR_NO_FRAME_AVAILABLE;
            response.errorMessage = "No video frame available";
            return response;
        }

        const auto &frame = *frameOpt;
        response.status = IpcStatus::OK;
        response.data = {
            {"width", frame.width},
            {"height", frame.height},
            {"format", frame.format},
            {"timestampUs", frame.timestampUs},
            {"frameData", base64Encode(frame.data)}};

        return response;
    }

    IpcResponse IpcServer::handleGetAudioFrame(const IpcRequest &request)
    {
        IpcResponse response;
        response.id = request.id;

        if (!sessionManager_)
        {
            response.status = IpcStatus::ERR_INTERNAL;
            response.errorMessage = "Session manager not available";
            return response;
        }

        // Use SessionManager's frame retrieval which uses registered callbacks
        auto frameOpt = sessionManager_->getAudioFrame(request.sessionId);
        if (!frameOpt)
        {
            response.status = IpcStatus::ERR_NO_FRAME_AVAILABLE;
            response.errorMessage = "No audio frame available";
            return response;
        }

        const auto &frame = *frameOpt;
        response.status = IpcStatus::OK;
        response.data = {
            {"sampleRate", frame.sampleRate},
            {"channels", frame.channels},
            {"samplesPerChannel", frame.frames},
            {"format", "float32"},
            {"timestampUs", frame.timestampUs},
            {"frameData", base64EncodeFloat(frame.data)}};

        return response;
    }

    IpcResponse IpcServer::handleGetSessionInfo(const IpcRequest &request)
    {
        IpcResponse response;
        response.id = request.id;

        if (!sessionManager_)
        {
            response.status = IpcStatus::ERR_INTERNAL;
            response.errorMessage = "Session manager not available";
            return response;
        }

        // Get basic session info from SessionManager
        auto sessionOpt = sessionManager_->getSession(request.sessionId);
        if (!sessionOpt)
        {
            response.status = IpcStatus::ERR_SESSION_NOT_FOUND;
            response.errorMessage = "Session not found";
            return response;
        }

        const auto &session = *sessionOpt;

        // Get extended info from callback if available (for fps, bitrate, codec, etc.)
        json extendedInfo;
        if (sessionInfoCallback_)
        {
            auto info = sessionInfoCallback_(request.sessionId);
            if (info)
            {
                extendedInfo = {
                    {"width", info->width},
                    {"height", info->height},
                    {"fps", info->fps},
                    {"bitrateKbps", info->bitrateKbps},
                    {"codec", info->codec},
                    {"connected", info->connected}};
            }
        }

        response.status = IpcStatus::OK;
        response.data = {
            {"sessionId", session.sessionId},
            {"deviceId", session.deviceId},
            {"videoEnabled", session.videoEnabled},
            {"audioEnabled", session.audioEnabled},
            {"muted", session.muted},
            {"gain", session.gain},
            {"audioDelayMs", session.audioDelayMs}};

        // Merge extended info if available
        for (const auto &[key, value] : extendedInfo.items())
        {
            response.data[key] = value;
        }

        return response;
    }

    IpcResponse IpcServer::handleSubscribe(const IpcRequest &request)
    {
        IpcResponse response;
        response.id = request.id;

        // For now, subscribe is a no-op - frames are pulled on demand
        // In future, could implement push-based subscription
        response.status = IpcStatus::OK;
        response.data = {{"subscribed", true}};
        return response;
    }

    IpcResponse IpcServer::handleUnsubscribe(const IpcRequest &request)
    {
        IpcResponse response;
        response.id = request.id;

        response.status = IpcStatus::OK;
        response.data = {{"unsubscribed", true}};
        return response;
    }

    bool IpcServer::validateToken(const std::string &sessionId, const std::string &token) const
    {
        if (!securityManager_)
        {
            return false;
        }

        // Use SecurityManager's cryptographic token validation
        return securityManager_->validateSessionToken(sessionId, token);
    }

    std::vector<uint8_t> IpcServer::serializeResponse(const IpcResponse &response)
    {
        json j;
        j["id"] = response.id;
        j["status"] = static_cast<uint32_t>(response.status);
        if (!response.data.empty())
        {
            j["data"] = response.data;
        }
        if (!response.errorMessage.empty())
        {
            j["error"] = response.errorMessage;
        }

        std::string jsonStr = j.dump();
        return std::vector<uint8_t>(jsonStr.begin(), jsonStr.end());
    }

    std::optional<IpcRequest> IpcServer::deserializeRequest(const std::vector<uint8_t> &data)
    {
        try
        {
            std::string jsonStr(data.begin(), data.end());
            json j = json::parse(jsonStr);

            IpcRequest request;
            request.id = j.value("id", 0);
            request.command = static_cast<IpcCommand>(j.value("cmd", static_cast<uint32_t>(IpcCommand::INVALID)));
            request.sessionId = j.value("sessionId", "");
            request.token = j.value("token", "");
            request.params = j.value("params", json::object());

            return request;
        }
        catch (const std::exception &e)
        {
            std::cerr << "[IpcServer] JSON parse error: " << e.what() << std::endl;
            return std::nullopt;
        }
    }

    std::string IpcServer::encodeBase64(const std::vector<uint8_t> &data)
    {
        return base64Encode(data);
    }

    std::string IpcServer::encodeAudioBase64(const std::vector<float> &data)
    {
        return base64EncodeFloat(data);
    }

    // ========================================================================
    // NEW METHODS: Push callback registration and frame delivery
    // ========================================================================

    void IpcServer::registerVideoPushCallback(VideoFramePushCallback callback)
    {
        videoPushCallback_ = std::move(callback);
        std::cout << "[IpcServer] Video push callback registered" << std::endl;
    }

    void IpcServer::registerAudioPushCallback(AudioFramePushCallback callback)
    {
        audioPushCallback_ = std::move(callback);
        std::cout << "[IpcServer] Audio push callback registered" << std::endl;
    }

    void IpcServer::registerSessionInfoCallback(SessionInfoCallback callback)
    {
        std::lock_guard<std::mutex> lock(pushCallbackMutex_);
        sessionInfoCallback_ = std::move(callback);
        std::cout << "[IpcServer] Session info callback registered" << std::endl;
    }

    void IpcServer::deliverVideoFrame(const std::string &sessionId, const VideoFrame &frame)
    {
        // If push callback is registered, use it
        if (videoPushCallback_)
        {
            videoPushCallback_(sessionId, frame);
        }

        // Also push to any connected OBS clients via named pipes
        // Create a push message for the frame
        json j;
        j["type"] = "video_frame";
        j["sessionId"] = sessionId;
        j["width"] = frame.width;
        j["height"] = frame.height;
        j["format"] = frame.format;
        j["timestampUs"] = frame.timestampUs;
        j["frameData"] = encodeBase64(frame.data);

        std::string jsonStr = j.dump();
        std::vector<uint8_t> message(jsonStr.begin(), jsonStr.end());

        // Send to all connected clients for this session
        std::lock_guard<std::mutex> lock(pipesMutex_);
        auto it = pipes_.find(sessionId);
        if (it != pipes_.end() && it->second && it->second->active)
        {
            writeMessage(*it->second, message);
        }
    }

    void IpcServer::deliverAudioFrame(const std::string &sessionId, const AudioFrame &frame)
    {
        // If push callback is registered, use it
        if (audioPushCallback_)
        {
            audioPushCallback_(sessionId, frame);
        }

        // Also push to any connected OBS clients via named pipes
        json j;
        j["type"] = "audio_frame";
        j["sessionId"] = sessionId;
        j["sampleRate"] = frame.sampleRate;
        j["channels"] = frame.channels;
        j["samplesPerChannel"] = frame.frames;
        j["format"] = "float32";
        j["timestampUs"] = frame.timestampUs;
        j["frameData"] = encodeAudioBase64(frame.data);

        std::string jsonStr = j.dump();
        std::vector<uint8_t> message(jsonStr.begin(), jsonStr.end());

        // Send to all connected clients for this session
        std::lock_guard<std::mutex> lock(pipesMutex_);
        auto it = pipes_.find(sessionId);
        if (it != pipes_.end() && it->second && it->second->active)
        {
            writeMessage(*it->second, message);
        }
    }

    void IpcServer::processMessages()
    {
        // This is called from the main loop to process any pending operations
        // For now, the IPC server handles messages in dedicated worker threads per pipe
        // This method can be used for periodic maintenance tasks

        // Clean up any disconnected pipes
        std::lock_guard<std::mutex> lock(pipesMutex_);
        for (auto it = pipes_.begin(); it != pipes_.end();)
        {
            if (it->second && !it->second->active)
            {
                it = pipes_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

} // namespace phonecam