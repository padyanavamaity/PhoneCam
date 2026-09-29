#pragma once

#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <unordered_map>

namespace phonecam {

// Discovered device information
struct DiscoveredDevice {
    std::string deviceId;       // From TXT record "id" - persistent PhoneCam ID
    std::string displayName;    // From TXT record "name" - user-friendly name
    std::string hostname;       // Resolved hostname from SRV record
    std::string ipAddress;      // Resolved IP address
    uint16_t port = 0;          // From SRV record
    std::string proto;          // From TXT record "proto" - signaling protocol version
    std::string path;           // From TXT record "path" - signaling path (e.g., "/ws")
    std::string serviceName;    // Full service name (e.g., "My Phone._phonecam._tcp.local")
    std::string serviceType;    // Service type (e.g., "_phonecam._tcp.local")
    
    // For comparison and deduplication
    bool operator==(const DiscoveredDevice& other) const {
        return deviceId == other.deviceId && hostname == other.hostname && port == other.port;
    }
};

// Device discovery state
enum class DeviceState {
    ADDED,      // New device discovered
    UPDATED,    // Existing device updated (e.g., IP changed)
    REMOVED     // Device no longer available
};

// Callback for device state changes
using DeviceDiscoveredCallback = std::function<void(const DiscoveredDevice& device, DeviceState state)>;

// Configuration for discovery client
struct DiscoveryClientConfig {
    std::string serviceType = "_phonecam._tcp.local";  // mDNS service type to browse
    std::chrono::milliseconds browseInterval{5000};    // How often to re-browse (0 = one-shot)
    bool resolveAddresses = true;                       // Whether to resolve hostnames to IPs
    std::chrono::seconds resolveTimeout{5};             // Timeout for address resolution
};

class DiscoveryClient {
public:
    DiscoveryClient();
    ~DiscoveryClient();

    // Non-copyable, movable
    DiscoveryClient(const DiscoveryClient&) = delete;
    DiscoveryClient& operator=(const DiscoveryClient&) = delete;
    DiscoveryClient(DiscoveryClient&&) = default;
    DiscoveryClient& operator=(DiscoveryClient&&) = default;

    // Start discovery with the given configuration
    bool start(const DiscoveryClientConfig& config = {});
    
    // Stop discovery
    void stop();

    // Check if discovery is running
    bool isRunning() const { return running_.load(); }

    // Set callback for device state changes
    void setOnDeviceCallback(DeviceDiscoveredCallback callback) { 
        onDeviceCallback_ = std::move(callback); 
    }

    // Get currently discovered devices
    std::vector<DiscoveredDevice> getDiscoveredDevices() const;

    // Trigger a manual browse (useful for one-shot or on-demand discovery)
    void browseOnce();

private:
    // Internal implementation using Windows DNS Service Discovery API
    class Impl;
    std::unique_ptr<Impl> impl_;

    DiscoveryClientConfig config_;
    std::atomic<bool> running_{false};
    DeviceDiscoveredCallback onDeviceCallback_;
};

} // namespace phonecam