#pragma once

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace phonecam {

/**
 * SecureStorage provides encrypted storage for sensitive data using Windows DPAPI.
 * 
 * Features:
 * - CryptProtectData / CryptUnprotectData for encryption
 * - Machine-level (CRYPTPROTECT_LOCAL_MACHINE) or user-level (CRYPTPROTECT_CURRENT_USER) protection
 * - Optional entropy for additional protection
 * - Stores master key, session tokens, pairing credentials
 * 
 * Requirements:
 * - Windows (Crypt32.lib)
 * - Link with Crypt32.lib in CMakeLists.txt
 */
class SecureStorage {
public:
    /**
     * Protection scope for DPAPI
     */
    enum class Scope {
        CurrentUser,  // CRYPTPROTECT_CURRENT_USER - only current user can decrypt
        LocalMachine  // CRYPTPROTECT_LOCAL_MACHINE - any user on machine can decrypt
    };

    /**
     * Initialize SecureStorage
     * @param scope Protection scope (default: CurrentUser)
     * @param entropy Optional additional entropy for encryption
     */
    explicit SecureStorage(
        Scope scope = Scope::CurrentUser,
        const std::vector<uint8_t>& entropy = {}
    );

    ~SecureStorage() = default;

    // Non-copyable, movable
    SecureStorage(const SecureStorage&) = delete;
    SecureStorage& operator=(const SecureStorage&) = delete;
    SecureStorage(SecureStorage&&) noexcept = default;
    SecureStorage& operator=(SecureStorage&&) noexcept = default;

    /**
     * Store the master key (32 bytes) for SecurityManager token signing
     * @param masterKey 32-byte master key
     * @return true on success
     */
    bool storeMasterKey(const std::vector<uint8_t>& masterKey);

    /**
     * Retrieve the master key for SecurityManager token signing
     * @return Master key (32 bytes) or empty if not found/failed
     */
    std::optional<std::vector<uint8_t>> retrieveMasterKey();

    /**
     * Check if master key exists in secure storage
     */
    bool hasMasterKey() const;

    /**
     * Store a session token (encrypted)
     * @param sessionId Session identifier
     * @param tokenData Token data to store
     * @return true on success
     */
    bool storeSessionToken(const std::string& sessionId, const std::vector<uint8_t>& tokenData);

    /**
     * Retrieve a session token
     * @param sessionId Session identifier
     * @return Token data or empty if not found/failed
     */
    std::optional<std::vector<uint8_t>> retrieveSessionToken(const std::string& sessionId);

    /**
     * Delete a session token
     */
    void deleteSessionToken(const std::string& sessionId);

    /**
     * Store pairing credentials for a device
     * @param deviceId Device identifier
     * @param credentials Credentials data to store
     * @return true on success
     */
    bool storePairingCredentials(const std::string& deviceId, const std::vector<uint8_t>& credentials);

    /**
     * Retrieve pairing credentials for a device
     * @param deviceId Device identifier
     * @return Credentials data or empty if not found/failed
     */
    std::optional<std::vector<uint8_t>> retrievePairingCredentials(const std::string& deviceId);

    /**
     * Delete pairing credentials
     */
    void deletePairingCredentials(const std::string& deviceId);

    /**
     * Clear all stored data
     */
    void clearAllData();

    /**
     * Check if secure storage is available
     */
    bool isAvailable() const;

    /**
     * Get info about the current configuration (for diagnostics)
     */
    struct Info {
        std::string scope;
        bool hasEntropy;
        std::string description;
    };
    Info getInfo() const;

private:
    // Internal data blob structure for storage
    struct DataBlob {
        std::vector<uint8_t> data;
    };

    // Encrypt data using DPAPI
    std::optional<DataBlob> protectData(const std::vector<uint8_t>& plaintext) const;

    // Decrypt data using DPAPI
    std::optional<std::vector<uint8_t>> unprotectData(const DataBlob& ciphertext) const;

    // Registry/storage path helpers
    std::string getMasterKeyPath() const;
    std::string getSessionTokenPath(const std::string& sessionId) const;
    std::string getPairingCredentialsPath(const std::string& deviceId) const;
    std::filesystem::path getStorageDirectory() const;

    // Write data blob to storage
    bool writeDataBlob(const std::string& path, const DataBlob& blob) const;

    // Read data blob from storage
    std::optional<DataBlob> readDataBlob(const std::string& path) const;

    // Delete data from storage
    void deleteDataBlob(const std::string& path) const;

    // Ensure storage directory exists
    void ensureStorageDirectory() const;

    Scope scope_;
    std::vector<uint8_t> entropy_;
    bool useLocalMachine_;
    DWORD cryptProtectFlags_;
};

} // namespace phonecam