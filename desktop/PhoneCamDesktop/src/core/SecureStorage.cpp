#include "phonecam/SecureStorage.h"

#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdlib>

namespace phonecam {

namespace fs = std::filesystem;

SecureStorage::SecureStorage(Scope scope, const std::vector<uint8_t>& entropy)
    : scope_(scope)
    , entropy_(entropy)
    , useLocalMachine_(scope == Scope::LocalMachine)
    , cryptProtectFlags_(useLocalMachine_ ? CRYPTPROTECT_LOCAL_MACHINE : 0) {
    // Storage directory will be created on first use
}

std::optional<SecureStorage::DataBlob> SecureStorage::protectData(
    const std::vector<uint8_t>& plaintext) const {
    
    DATA_BLOB inBlob;
    inBlob.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(plaintext.data()));
    inBlob.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB entropyBlob;
    DATA_BLOB* pEntropyBlob = nullptr;
    if (!entropy_.empty()) {
        entropyBlob.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(entropy_.data()));
        entropyBlob.cbData = static_cast<DWORD>(entropy_.size());
        pEntropyBlob = &entropyBlob;
    }

    DATA_BLOB outBlob;
    BOOL result = CryptProtectData(
        &inBlob,
        L"PhoneCam SecureStorage",  // Description (optional)
        pEntropyBlob,
        nullptr,                    // Reserved
        nullptr,                    // Prompt struct (optional)
        cryptProtectFlags_,
        &outBlob
    );

    if (!result) {
        GetLastError(); // Suppress unused variable warning
        // Log error if needed
        return std::nullopt;
    }

    DataBlob blob;
    blob.data.assign(outBlob.pbData, outBlob.pbData + outBlob.cbData);
    
    // Free the output blob allocated by CryptProtectData
    LocalFree(outBlob.pbData);
    
    return blob;
}

std::optional<std::vector<uint8_t>> SecureStorage::unprotectData(
    const DataBlob& ciphertext) const {
    
    DATA_BLOB inBlob;
    inBlob.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(ciphertext.data.data()));
    inBlob.cbData = static_cast<DWORD>(ciphertext.data.size());

    DATA_BLOB entropyBlob;
    DATA_BLOB* pEntropyBlob = nullptr;
    if (!entropy_.empty()) {
        entropyBlob.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(entropy_.data()));
        entropyBlob.cbData = static_cast<DWORD>(entropy_.size());
        pEntropyBlob = &entropyBlob;
    }

    DATA_BLOB outBlob;
    BOOL result = CryptUnprotectData(
        &inBlob,
        nullptr,                    // Description output (optional)
        pEntropyBlob,
        nullptr,                    // Reserved
        nullptr,                    // Prompt struct (optional)
        0,                          // Flags (CRYPTPROTECT_LOCAL_MACHINE not needed for unprotect)
        &outBlob
    );

    if (!result) {
        GetLastError(); // Suppress unused variable warning
        // Log error if needed
        return std::nullopt;
    }

    std::vector<uint8_t> plaintext(outBlob.pbData, outBlob.pbData + outBlob.cbData);
    
    // Free the output blob allocated by CryptUnprotectData
    LocalFree(outBlob.pbData);
    
    return plaintext;
}

std::filesystem::path SecureStorage::getStorageDirectory() const {
    PWSTR pathPtr = nullptr;
    // Use local app data for user scope, program data for machine scope
    HRESULT hr = SHGetKnownFolderPath(
        useLocalMachine_ ? FOLDERID_ProgramData : FOLDERID_LocalAppData,
        0, nullptr, &pathPtr);
    
    std::filesystem::path baseDir;
    if (SUCCEEDED(hr) && pathPtr) {
        baseDir = pathPtr;
        CoTaskMemFree(pathPtr);
    } else {
        // Fallback
        char* localAppData = nullptr;
        size_t len = 0;
        _dupenv_s(&localAppData, &len, "LOCALAPPDATA");
        std::string localAppDataStr = localAppData ? localAppData : "";
        free(localAppData);
        baseDir = useLocalMachine_
            ? std::filesystem::path("C:/ProgramData/PhoneCam")
            : (localAppDataStr.empty() ? std::filesystem::path("") : std::filesystem::path(localAppDataStr)) / "PhoneCam";
    }
    
    return baseDir / "PhoneCam" / "SecureStorage";
}

std::string SecureStorage::getMasterKeyPath() const {
    return (getStorageDirectory() / "master_key.bin").string();
}

std::string SecureStorage::getSessionTokenPath(const std::string& sessionId) const {
    // Sanitize sessionId for filesystem
    std::string safeId = sessionId;
    std::replace(safeId.begin(), safeId.end(), '/', '_');
    std::replace(safeId.begin(), safeId.end(), '\\', '_');
    std::replace(safeId.begin(), safeId.end(), ':', '_');
    return (getStorageDirectory() / ("session_" + safeId + ".bin")).string();
}

std::string SecureStorage::getPairingCredentialsPath(const std::string& deviceId) const {
    // Sanitize deviceId for filesystem
    std::string safeId = deviceId;
    std::replace(safeId.begin(), safeId.end(), '/', '_');
    std::replace(safeId.begin(), safeId.end(), '\\', '_');
    std::replace(safeId.begin(), safeId.end(), ':', '_');
    return (getStorageDirectory() / ("pairing_" + safeId + ".bin")).string();
}

bool SecureStorage::writeDataBlob(const std::string& path, const DataBlob& blob) const {
    try {
        ensureStorageDirectory();
        
        // Write to temporary file first, then rename for atomicity
        fs::path filePath(path);
        fs::path tempPath = filePath;
        tempPath += ".tmp";
        
        std::ofstream ofs(tempPath, std::ios::binary);
        if (!ofs) {
            return false;
        }
        ofs.write(reinterpret_cast<const char*>(blob.data.data()), blob.data.size());
        ofs.close();
        
        if (!ofs) {
            fs::remove(tempPath);
            return false;
        }
        
        // Atomic rename
        fs::rename(tempPath, filePath);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<SecureStorage::DataBlob> SecureStorage::readDataBlob(const std::string& path) const {
    try {
        fs::path filePath(path);
        if (!fs::exists(filePath)) {
            return std::nullopt;
        }
        
        std::ifstream ifs(filePath, std::ios::binary | std::ios::ate);
        if (!ifs) {
            return std::nullopt;
        }
        
        std::streamsize size = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        
        if (size <= 0) {
            return std::nullopt;
        }
        
        DataBlob blob;
        blob.data.resize(static_cast<size_t>(size));
        ifs.read(reinterpret_cast<char*>(blob.data.data()), size);
        
        if (!ifs) {
            return std::nullopt;
        }
        
        return blob;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void SecureStorage::deleteDataBlob(const std::string& path) const {
    try {
        fs::remove(path);
    } catch (const std::exception&) {
        // Ignore
    }
}

bool SecureStorage::storeMasterKey(const std::vector<uint8_t>& masterKey) {
    if (masterKey.size() != 32) {
        return false; // Must be 32 bytes (256 bits)
    }
    
    auto encrypted = protectData(masterKey);
    if (!encrypted) {
        return false;
    }
    
    return writeDataBlob(getMasterKeyPath(), *encrypted);
}

std::optional<std::vector<uint8_t>> SecureStorage::retrieveMasterKey() {
    auto blob = readDataBlob(getMasterKeyPath());
    if (!blob) {
        return std::nullopt;
    }
    
    return unprotectData(*blob);
}

bool SecureStorage::hasMasterKey() const {
    return fs::exists(getMasterKeyPath());
}

bool SecureStorage::storeSessionToken(const std::string& sessionId, 
                                       const std::vector<uint8_t>& tokenData) {
    auto encrypted = protectData(tokenData);
    if (!encrypted) {
        return false;
    }
    
    return writeDataBlob(getSessionTokenPath(sessionId), *encrypted);
}

std::optional<std::vector<uint8_t>> SecureStorage::retrieveSessionToken(
    const std::string& sessionId) {
    auto blob = readDataBlob(getSessionTokenPath(sessionId));
    if (!blob) {
        return std::nullopt;
    }
    
    return unprotectData(*blob);
}

void SecureStorage::deleteSessionToken(const std::string& sessionId) {
    deleteDataBlob(getSessionTokenPath(sessionId));
}

bool SecureStorage::storePairingCredentials(const std::string& deviceId,
                                             const std::vector<uint8_t>& credentials) {
    auto encrypted = protectData(credentials);
    if (!encrypted) {
        return false;
    }
    
    return writeDataBlob(getPairingCredentialsPath(deviceId), *encrypted);
}

std::optional<std::vector<uint8_t>> SecureStorage::retrievePairingCredentials(
    const std::string& deviceId) {
    auto blob = readDataBlob(getPairingCredentialsPath(deviceId));
    if (!blob) {
        return std::nullopt;
    }
    
    return unprotectData(*blob);
}

void SecureStorage::deletePairingCredentials(const std::string& deviceId) {
    deleteDataBlob(getPairingCredentialsPath(deviceId));
}

void SecureStorage::clearAllData() {
    try {
        fs::path storageDir = getStorageDirectory();
        if (fs::exists(storageDir)) {
            for (const auto& entry : fs::directory_iterator(storageDir)) {
                if (entry.is_regular_file()) {
                    fs::remove(entry.path());
                }
            }
        }
    } catch (const std::exception&) {
        // Ignore
    }
}

bool SecureStorage::isAvailable() const {
    // Test DPAPI availability with a small test
    std::vector<uint8_t> testData = {'t', 'e', 's', 't'};
    auto encrypted = protectData(testData);
    if (!encrypted) {
        return false;
    }
    
    auto decrypted = unprotectData(*encrypted);
    return decrypted && *decrypted == testData;
}

SecureStorage::Info SecureStorage::getInfo() const {
    return Info{
        .scope = useLocalMachine_ ? "LocalMachine" : "CurrentUser",
        .hasEntropy = !entropy_.empty(),
        .description = "Windows DPAPI (CryptProtectData/CryptUnprotectData)"
    };
}

void SecureStorage::ensureStorageDirectory() const {
    fs::path storageDir = getStorageDirectory();
    if (!fs::exists(storageDir)) {
        fs::create_directories(storageDir);
    }
}

} // namespace phonecam