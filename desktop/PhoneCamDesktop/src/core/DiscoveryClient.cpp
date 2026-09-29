#include "phonecam/DiscoveryClient.h"

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windns.h>
#include <iphlpapi.h>
#include <stdio.h>

#pragma comment(lib, "dnsapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#include <iostream>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>
#include <algorithm>
#include <sstream>
#include <deque>

namespace phonecam {

// Helper to convert wide string to UTF-8
static std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string str(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &str[0], size, nullptr, nullptr);
    return str;
}

// Helper to convert ANSI string to UTF-8 (no-op on Windows since ANSI is already single-byte)
static std::string AnsiToUtf8(const char* astr) {
    if (!astr) return "";
    return std::string(astr);
}

// Helper to convert UTF-8 to wide string
static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (size <= 0) return L"";
    std::wstring wstr(size - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size);
    return wstr;
}

// Helper to extract TXT record value by key
static std::string ExtractTxtValue(const std::string& txtData, const std::string& key) {
    // TXT records are typically key=value pairs, space or null separated
    std::string searchKey = key + "=";
    size_t pos = txtData.find(searchKey);
    if (pos == std::string::npos) return "";
    
    pos += searchKey.length();
    size_t end = txtData.find_first_of(" \0", pos);
    if (end == std::string::npos) end = txtData.length();
    
    return txtData.substr(pos, end - pos);
}

// Helper to parse TXT record from DNS_RECORD linked list
static std::string ParseTxtRecord(const DNS_RECORD* record) {
    if (!record) return "";
    
    std::string result;
    for (const DNS_RECORD* r = record; r; r = r->pNext) {
        if (r->wType == DNS_TYPE_TEXT) {
            // Use the wide version (DNS_TXT_DATAW) since DnsQuery_W returns DNS_RECORDW
            // DNS_RECORD is typedef'd to DNS_RECORDA, but DnsQuery_W returns DNS_RECORDW*
            // Cast to access the wide version
            const DNS_TXT_DATAW* txt = reinterpret_cast<const DNS_TXT_DATAW*>(&r->Data);
            for (DWORD i = 0; i < txt->dwStringCount; ++i) {
                if (txt->pStringArray[i]) {
                    if (!result.empty()) result += " ";
                    result += WideToUtf8(txt->pStringArray[i]);
                }
            }
        }
    }
    return result;
}

// Helper to extract A/AAAA record data
static std::string ExtractAddressRecord(const DNS_RECORD* record) {
    if (!record) return "";
    
    for (const DNS_RECORD* r = record; r; r = r->pNext) {
        if (r->wType == DNS_TYPE_A) {
            char ipStr[INET_ADDRSTRLEN];
            in_addr addr;
            addr.S_un.S_addr = r->Data.A.IpAddress;
            if (inet_ntop(AF_INET, &addr, ipStr, sizeof(ipStr))) {
                return std::string(ipStr);
            }
        } else if (r->wType == DNS_TYPE_AAAA) {
            char ipStr[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, &r->Data.AAAA.Ip6Address, ipStr, sizeof(ipStr))) {
                return std::string(ipStr);
            }
        }
    }
    return "";
}

// Internal implementation using Windows DNS Query API for mDNS
class DiscoveryClient::Impl {
public:
    Impl() : running_(false) {}
    ~Impl() { cleanup(); }

    bool start(const DiscoveryClientConfig& config, DeviceDiscoveredCallback callback) {
        config_ = config;
        onDeviceCallback_ = std::move(callback);
        running_ = true;
        
        // Initialize Winsock if needed
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "[DiscoveryClient] WSAStartup failed" << std::endl;
            return false;
        }
        winsockInitialized_ = true;

        // Start browse thread
        browseThread_ = std::thread(&Impl::browseLoop, this);
        std::cout << "[DiscoveryClient] Started browsing for " << config_.serviceType << std::endl;
        return true;
    }

    void stop() {
        running_ = false;
        
        if (browseThread_.joinable()) {
            browseThread_.join();
        }
        
        // Stop any pending resolve operations
        {
            std::lock_guard<std::mutex> lock(resolveMutex_);
            resolveThreadRunning_ = false;
        }
        resolveCV_.notify_all();
        
        if (resolveThread_.joinable()) {
            resolveThread_.join();
        }
        
        cleanup();
    }

    std::vector<DiscoveredDevice> getDevices() const {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        std::vector<DiscoveredDevice> result;
        result.reserve(devices_.size());
        for (const auto& [key, device] : devices_) {
            result.push_back(device);
        }
        return result;
    }

    void browseOnce() {
        // Trigger an immediate browse
        browseCV_.notify_one();
    }

private:
    void cleanup() {
        if (winsockInitialized_) {
            WSACleanup();
            winsockInitialized_ = false;
        }
        
        std::lock_guard<std::mutex> lock(devicesMutex_);
        devices_.clear();
    }

    void browseLoop() {
        while (running_) {
            // Query for _phonecam._tcp.local SRV records using mDNS
            queryMdnsServices();
            
            // Wait for next browse interval or stop signal
            std::unique_lock<std::mutex> lock(browseMutex_);
            if (config_.browseInterval.count() > 0) {
                browseCV_.wait_for(lock, config_.browseInterval, [this] { return !running_; });
            } else {
                // One-shot: wait indefinitely until stopped
                browseCV_.wait(lock, [this] { return !running_; });
            }
            
            if (!running_) break;
        }
    }

    void queryMdnsServices() {
        // Query for SRV records for the service type (e.g., _phonecam._tcp.local)
        std::wstring serviceTypeWide = Utf8ToWide(config_.serviceType);
        
        DNS_RECORD* srvRecords = nullptr;
        DNS_STATUS status = DnsQuery_W(
            serviceTypeWide.c_str(),
            DNS_TYPE_SRV,
            DNS_QUERY_STANDARD | DNS_QUERY_BYPASS_CACHE,
            nullptr,  // Use default DNS servers (includes mDNS)
            &srvRecords,
            nullptr
        );
        
        if (status != ERROR_SUCCESS || !srvRecords) {
            // No SRV records found or error
            if (srvRecords) DnsRecordListFree(srvRecords, DnsFreeRecordList);
            std::cout << "[DiscoveryClient] No SRV records found for " << config_.serviceType << " (status=" << status << ")" << std::endl;
            return;
        }
        
        // Process each SRV record
        for (DNS_RECORD* srv = srvRecords; srv; srv = srv->pNext) {
            if (srv->wType != DNS_TYPE_SRV) continue;
            
            std::string serviceName = AnsiToUtf8(srv->pName);
            std::string targetHost = AnsiToUtf8(srv->Data.SRV.pNameTarget);
            uint16_t port = srv->Data.SRV.wPort;
            uint16_t priority = srv->Data.SRV.wPriority;
            uint16_t weight = srv->Data.SRV.wWeight;
            
            (void)priority; (void)weight; // Currently unused
            
            // Now query for TXT record for this service instance
            std::string txtData = queryTxtRecord(serviceName);
            
            // Extract device info from TXT record
            std::string deviceId = ExtractTxtValue(txtData, "id");
            std::string displayName = ExtractTxtValue(txtData, "name");
            std::string proto = ExtractTxtValue(txtData, "proto");
            std::string path = ExtractTxtValue(txtData, "path");
            
            // If no device ID in TXT, generate from service name
            if (deviceId.empty()) {
                size_t dotPos = serviceName.find('.');
                if (dotPos != std::string::npos) {
                    deviceId = serviceName.substr(0, dotPos);
                } else {
                    deviceId = serviceName;
                }
            }
            
            // If no display name, use instance name
            if (displayName.empty()) {
                displayName = serviceName;
            }
            
            // Query for A/AAAA records for the target host
            std::string ipAddress = queryAddressRecord(targetHost);
            
            // Create device entry
            DiscoveredDevice device;
            device.deviceId = deviceId;
            device.displayName = displayName;
            device.hostname = targetHost;
            device.ipAddress = ipAddress;
            device.port = port;
            device.proto = proto;
            device.path = path;
            device.serviceName = serviceName;
            device.serviceType = config_.serviceType;
            
            // Add/update device
            addOrUpdateDevice(device, DeviceState::ADDED);
            
            std::cout << "[DiscoveryClient] Device discovered: " << device.displayName
                      << " (ID: " << device.deviceId
                      << ", Host: " << device.hostname
                      << ", IP: " << device.ipAddress
                      << ", Port: " << device.port
                      << ", Proto: " << device.proto
                      << ", Path: " << device.path << ")" << std::endl;
        }
        
        DnsRecordListFree(srvRecords, DnsFreeRecordList);
    }
    
    std::string queryTxtRecord(const std::string& serviceName) {
        std::wstring serviceNameWide = Utf8ToWide(serviceName);
        
        DNS_RECORD* txtRecords = nullptr;
        DNS_STATUS status = DnsQuery_W(
            serviceNameWide.c_str(),
            DNS_TYPE_TEXT,
            DNS_QUERY_STANDARD | DNS_QUERY_BYPASS_CACHE,
            nullptr,
            &txtRecords,
            nullptr
        );
        
        if (status != ERROR_SUCCESS || !txtRecords) {
            if (txtRecords) DnsRecordListFree(txtRecords, DnsFreeRecordList);
            return "";
        }
        
        std::string result = ParseTxtRecord(txtRecords);
        DnsRecordListFree(txtRecords, DnsFreeRecordList);
        return result;
    }
    
    std::string queryAddressRecord(const std::string& hostname) {
        std::wstring hostnameWide = Utf8ToWide(hostname);
        
        // Try A record first
        DNS_RECORD* aRecords = nullptr;
        DNS_STATUS status = DnsQuery_W(
            hostnameWide.c_str(),
            DNS_TYPE_A,
            DNS_QUERY_STANDARD | DNS_QUERY_BYPASS_CACHE,
            nullptr,
            &aRecords,
            nullptr
        );
        
        if (status == ERROR_SUCCESS && aRecords) {
            std::string result = ExtractAddressRecord(aRecords);
            DnsRecordListFree(aRecords, DnsFreeRecordList);
            if (!result.empty()) return result;
        }
        
        // Try AAAA record
        DNS_RECORD* aaaaRecords = nullptr;
        status = DnsQuery_W(
            hostnameWide.c_str(),
            DNS_TYPE_AAAA,
            DNS_QUERY_STANDARD | DNS_QUERY_BYPASS_CACHE,
            nullptr,
            &aaaaRecords,
            nullptr
        );
        
        if (status == ERROR_SUCCESS && aaaaRecords) {
            std::string result = ExtractAddressRecord(aaaaRecords);
            DnsRecordListFree(aaaaRecords, DnsFreeRecordList);
            if (!result.empty()) return result;
        }
        
        // Fallback: use getaddrinfo
        return resolveHostnameSync(hostname);
    }

    void resolveHostname(DiscoveredDevice& device) {
        // Queue for async resolution
        {
            std::lock_guard<std::mutex> lock(resolveMutex_);
            pendingResolves_.push_back(device);
        }
        resolveCV_.notify_one();
        
        // Start resolve thread if not running
        if (!resolveThreadRunning_) {
            resolveThreadRunning_ = true;
            resolveThread_ = std::thread(&Impl::resolveLoop, this);
        }
    }

    void resolveLoop() {
        while (resolveThreadRunning_) {
            DiscoveredDevice device;
            bool hasWork = false;
            
            {
                std::unique_lock<std::mutex> lock(resolveMutex_);
                resolveCV_.wait_for(lock, std::chrono::seconds(1), [this] {
                    return !pendingResolves_.empty() || !resolveThreadRunning_;
                });
                
                if (!pendingResolves_.empty()) {
                    device = std::move(pendingResolves_.front());
                    pendingResolves_.pop_front();
                    hasWork = true;
                }
            }
            
            if (!hasWork) continue;
            if (!resolveThreadRunning_) break;

            // Perform DNS resolution
            std::string ipAddress = resolveHostnameSync(device.hostname);
            if (!ipAddress.empty()) {
                device.ipAddress = ipAddress;
                std::cout << "[DiscoveryClient] Device resolved: " << device.deviceId
                          << " (" << device.hostname << " -> " << ipAddress << ")" << std::endl;
            } else {
                std::cerr << "[DiscoveryClient] Failed to resolve hostname: " << device.hostname
                          << " for device: " << device.deviceId << std::endl;
            }
            
            // Add/update device with resolved IP
            addOrUpdateDevice(device, DeviceState::ADDED);
        }
    }

    std::string resolveHostnameSync(const std::string& hostname) {
        // Use getaddrinfo for resolution
        addrinfo hints = {}, *result = nullptr;
        hints.ai_family = AF_UNSPEC;    // IPv4 or IPv6
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        std::wstring whostname = Utf8ToWide(hostname);
        std::string ahostname = WideToUtf8(whostname.c_str());
        
        int ret = getaddrinfo(ahostname.c_str(), nullptr, &hints, &result);
        if (ret != 0 || !result) {
            return "";
        }

        std::string ipAddress;
        for (addrinfo* rp = result; rp; rp = rp->ai_next) {
            char ipStr[INET6_ADDRSTRLEN];
            void* addr;
            
            if (rp->ai_family == AF_INET) {
                addr = &reinterpret_cast<sockaddr_in*>(rp->ai_addr)->sin_addr;
            } else if (rp->ai_family == AF_INET6) {
                addr = &reinterpret_cast<sockaddr_in6*>(rp->ai_addr)->sin6_addr;
            } else {
                continue;
            }
            
            if (inet_ntop(rp->ai_family, addr, ipStr, sizeof(ipStr))) {
                ipAddress = ipStr;
                break;  // Use first resolved address
            }
        }
        
        freeaddrinfo(result);
        return ipAddress;
    }

    void addOrUpdateDevice(const DiscoveredDevice& device, DeviceState /*state*/) {
        std::string key = device.deviceId + "@" + device.hostname + ":" + std::to_string(device.port);
        
        std::lock_guard<std::mutex> lock(devicesMutex_);
        auto it = devices_.find(key);
        
        if (it == devices_.end()) {
            // New device
            devices_[key] = device;
            if (onDeviceCallback_) {
                onDeviceCallback_(device, DeviceState::ADDED);
            }
            std::cout << "[DiscoveryClient] Device added: " << device.displayName
                      << " (ID: " << device.deviceId
                      << ", IP: " << device.ipAddress
                      << ", Port: " << device.port << ")" << std::endl;
        } else {
            // Check if device info changed
            bool changed = (it->second.ipAddress != device.ipAddress ||
                           it->second.port != device.port ||
                           it->second.displayName != device.displayName ||
                           it->second.proto != device.proto ||
                           it->second.path != device.path);
            
            if (changed) {
                it->second = device;
                if (onDeviceCallback_) {
                    onDeviceCallback_(device, DeviceState::UPDATED);
                }
                std::cout << "[DiscoveryClient] Device updated: " << device.displayName
                          << " (ID: " << device.deviceId
                          << ", IP: " << device.ipAddress
                          << ", Port: " << device.port << ")" << std::endl;
            }
        }
    }

    // Configuration
    DiscoveryClientConfig config_;
    
    // State
    std::atomic<bool> running_{false};
    bool winsockInitialized_ = false;
    
    // Threads
    std::thread browseThread_;
    std::thread resolveThread_;
    std::atomic<bool> resolveThreadRunning_{false};
    
    // Synchronization
    mutable std::mutex devicesMutex_;
    std::mutex browseMutex_;
    std::condition_variable browseCV_;
    std::mutex resolveMutex_;
    std::condition_variable resolveCV_;
    std::deque<DiscoveredDevice> pendingResolves_;
    
    // Discovered devices (key = deviceId@hostname:port)
    std::unordered_map<std::string, DiscoveredDevice> devices_;
    
    // Callback
    DeviceDiscoveredCallback onDeviceCallback_;
};

DiscoveryClient::DiscoveryClient() : impl_(std::make_unique<Impl>()) {}
DiscoveryClient::~DiscoveryClient() { stop(); }

bool DiscoveryClient::start(const DiscoveryClientConfig& config) {
    return impl_->start(config, onDeviceCallback_);
}

void DiscoveryClient::stop() {
    impl_->stop();
}

std::vector<DiscoveredDevice> DiscoveryClient::getDiscoveredDevices() const {
    return impl_->getDevices();
}

void DiscoveryClient::browseOnce() {
    impl_->browseOnce();
}

} // namespace phonecam

#else
// Non-Windows stub implementation
namespace phonecam {

class DiscoveryClient::Impl {
public:
    bool start(const DiscoveryClientConfig&, DeviceDiscoveredCallback) { return false; }
    void stop() {}
    std::vector<DiscoveredDevice> getDevices() const { return {}; }
    void browseOnce() {}
};

DiscoveryClient::DiscoveryClient() : impl_(std::make_unique<Impl>()) {}
DiscoveryClient::~DiscoveryClient() = default;

bool DiscoveryClient::start(const DiscoveryClientConfig& config) {
    return impl_->start(config, onDeviceCallback_);
}

void DiscoveryClient::stop() {
    impl_->stop();
}

std::vector<DiscoveredDevice> DiscoveryClient::getDiscoveredDevices() const {
    return impl_->getDevices();
}

void DiscoveryClient::browseOnce() {
    impl_->browseOnce();
}

void DiscoveryClient::setOnDeviceCallback(DeviceDiscoveredCallback callback) {
    onDeviceCallback_ = std::move(callback);
}

} // namespace phonecam
#endif // _WIN32