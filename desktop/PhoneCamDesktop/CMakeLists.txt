cmake_minimum_required(VERSION 3.24)
project(PhoneCamDesktop LANGUAGES CXX)
cmake_policy(SET CMP0158 NEW)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Add nlohmann/json via FetchContent
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# WebRTC dependency configuration
# Supports multiple installation methods:
# 1. System package (find_package)
# 2. vcpkg (find_package with vcpkg toolchain)
# 3. Conan (conan_cmake_run)
# 4. Prebuilt WebRTC (set WEBRTC_ROOT cache variable)
# 5. Stub implementation (fallback for development)

option(WEBRTC_USE_VCPKG "Use vcpkg for WebRTC" OFF)
option(WEBRTC_USE_CONAN "Use Conan for WebRTC" OFF)
option(WEBRTC_USE_SYSTEM "Use system WebRTC package" OFF)
option(WEBRTC_USE_PREBUILT "Use prebuilt WebRTC from WEBRTC_ROOT" OFF)

# WebRTC root directory for prebuilt libraries
set(WEBRTC_ROOT "" CACHE PATH "Path to prebuilt WebRTC installation")

# Try to find WebRTC using various methods
set(WebRTC_FOUND FALSE)

# Method 1: Prebuilt WebRTC from WEBRTC_ROOT
if(WEBRTC_USE_PREBUILT AND WEBRTC_ROOT AND NOT WebRTC_FOUND)
    message(STATUS "Looking for prebuilt WebRTC at: ${WEBRTC_ROOT}")
    set(WebRTC_INCLUDE_DIRS "${WEBRTC_ROOT}/include")
    set(WebRTC_LIBRARY_DIRS "${WEBRTC_ROOT}/lib")
    
    # Find WebRTC libraries
    find_library(WEBRTC_LIB_peerconnection peerconnection PATHS "${WebRTC_LIBRARY_DIRS}" NO_DEFAULT_PATH)
    find_library(WEBRTC_LIB_webrtc webrtc PATHS "${WebRTC_LIBRARY_DIRS}" NO_DEFAULT_PATH)
    
    if(WEBRTC_LIB_peerconnection OR WEBRTC_LIB_webrtc)
        set(WebRTC_FOUND TRUE)
        set(WebRTC_INCLUDE_DIRS "${WEBRTC_ROOT}/include")
        if(WEBRTC_LIB_peerconnection)
            set(WebRTC_LIBRARIES ${WEBRTC_LIB_peerconnection})
        else()
            set(WebRTC_LIBRARIES ${WEBRTC_LIB_webrtc})
        endif()
        message(STATUS "Found prebuilt WebRTC at ${WEBRTC_ROOT}")
    endif()
endif()

# Method 2: vcpkg (find_package with vcpkg toolchain file)
if(WEBRTC_USE_VCPKG AND NOT WebRTC_FOUND)
    find_package(webrtc QUIET)
    if(webrtc_FOUND)
        set(WebRTC_FOUND TRUE)
        set(WebRTC_INCLUDE_DIRS ${webrtc_INCLUDE_DIRS})
        set(WebRTC_LIBRARIES ${webrtc_LIBRARIES})
        message(STATUS "Found WebRTC via vcpkg")
    endif()
endif()

# Method 3: Conan
if(WEBRTC_USE_CONAN AND NOT WebRTC_FOUND)
    # Conan integration would be handled via conan_cmake_run in toolchain
    # For now, try find_package
    find_package(WebRTC QUIET)
    if(WebRTC_FOUND)
        set(WebRTC_FOUND TRUE)
        message(STATUS "Found WebRTC via Conan")
    endif()
endif()

# Method 4: System package (standard find_package)
if(WEBRTC_USE_SYSTEM AND NOT WebRTC_FOUND)
    find_package(WebRTC QUIET)
    if(WebRTC_FOUND)
        set(WebRTC_FOUND TRUE)
        set(WebRTC_LIBRARIES ${WebRTC_LIBRARIES})
        message(STATUS "Found system WebRTC: ${WebRTC_VERSION}")
    endif()
endif()

# Method 5: Try standard find_package as fallback
if(NOT WebRTC_FOUND)
    find_package(WebRTC QUIET)
    if(WebRTC_FOUND)
        set(WebRTC_FOUND TRUE)
        set(WebRTC_LIBRARIES ${WebRTC_LIBRARIES})
        message(STATUS "Found WebRTC via find_package: ${WebRTC_VERSION}")
    endif()
endif()

if(NOT WebRTC_FOUND)
    message(WARNING "WebRTC not found. Using stub implementation.")
    message(WARNING "For production, enable one of: WEBRTC_USE_VCPKG, WEBRTC_USE_CONAN, WEBRTC_USE_SYSTEM, WEBRTC_USE_PREBUILT")
    message(WARNING "Or install WebRTC and set WebRTC_DIR / WEBRTC_ROOT")
    
    # Define a stub target for compilation without WebRTC
    add_library(webrtc_stub INTERFACE)
    target_compile_definitions(webrtc_stub INTERFACE PHONECAM_WEBRTC_STUB=1)
    set(WEBRTC_LIBRARIES webrtc_stub)
    set(WebRTC_INCLUDE_DIRS "")
else()
    message(STATUS "WebRTC configuration complete")
endif()

add_executable(PhoneCamDesktop
    src/main.cpp
    src/core/SessionManager.cpp
    src/core/SecurityManager.cpp
    src/core/SecureStorage.cpp
    src/core/AudioDistributor.cpp
    src/core/IpcServer.cpp
    src/core/WebRtcReceiver.cpp
    src/core/SignalingServer.cpp
)

target_include_directories(PhoneCamDesktop PRIVATE include ${WebRTC_INCLUDE_DIRS})

# Link OpenSSL libraries, nlohmann_json, and WebRTC
target_link_libraries(PhoneCamDesktop PRIVATE 
    OpenSSL::SSL 
    OpenSSL::Crypto 
    nlohmann_json::nlohmann_json
    ${WEBRTC_LIBRARIES}
)

# Add unit tests
add_executable(SecurityManagerTests
    tests/SecurityManagerTests.cpp
    src/core/SecurityManager.cpp
    src/core/SecureStorage.cpp
    src/core/SessionManager.cpp
    src/core/AudioDistributor.cpp
    src/core/WebRtcReceiver.cpp
)

target_include_directories(SecurityManagerTests PRIVATE include)
target_link_libraries(SecurityManagerTests PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    ${WEBRTC_LIBRARIES}
)

# Apply same platform settings to tests
if(WIN32)
    target_compile_definitions(SecurityManagerTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(SecurityManagerTests PRIVATE ws2_32 crypt32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(SecurityManagerTests PRIVATE pthread dl rt)
elseif(APPLE)
    target_link_libraries(SecurityManagerTests PRIVATE pthread)
endif()

# Register tests with CTest
enable_testing()
add_test(NAME SecurityManagerTests COMMAND SecurityManagerTests)

# Apply same security hardening to tests
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(SecurityManagerTests PRIVATE
        -Wall -Wextra -Wpedantic
        -Wformat=2 -Wformat-security
        # -Wstack-protector (valid GCC/Clang warning); the previous
        # "-Wstack-protector-strong" is NOT a real option and is silently
        # accepted then dropped, giving a false sense of coverage.
        -Wstack-protector
        -fstack-protector-strong
        -D_FORTIFY_SOURCE=2
    )
    target_link_options(SecurityManagerTests PRIVATE
        -Wl,-z,relro,-z,now
    )
elseif(MSVC)
    target_compile_options(SecurityManagerTests PRIVATE
        /W4 /WX
        /sdl
        /guard:cf
    )
    target_link_options(SecurityManagerTests PRIVATE
        /DYNAMICBASE /NXCOMPAT
    )
endif()

# Platform-specific settings
if(WIN32)
    # Windows-specific settings
    target_compile_definitions(PhoneCamDesktop PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    # Link Windows libraries if needed
    target_link_libraries(PhoneCamDesktop PRIVATE ws2_32 crypt32)
elseif(UNIX AND NOT APPLE)
    # Linux-specific settings
    target_link_libraries(PhoneCamDesktop PRIVATE pthread dl rt)
elseif(APPLE)
    # macOS-specific settings
    target_link_libraries(PhoneCamDesktop PRIVATE pthread)
endif()

# Security hardening flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(PhoneCamDesktop PRIVATE
        -Wall -Wextra -Wpedantic
        -Wformat=2 -Wformat-security
        -Wstack-protector
        -fstack-protector-strong
        -D_FORTIFY_SOURCE=2
    )
    target_link_options(PhoneCamDesktop PRIVATE
        -Wl,-z,relro,-z,now
    )
elseif(MSVC)
    target_compile_options(PhoneCamDesktop PRIVATE
        /W4 /WX
        /sdl
        /guard:cf
    )
    target_link_options(PhoneCamDesktop PRIVATE
        /DYNAMICBASE /NXCOMPAT
    )
endif()

# Installation
include(GNUInstallDirs)

install(TARGETS PhoneCamDesktop
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(TARGET SecurityManagerTests)
    install(TARGETS SecurityManagerTests
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/tests
    )
endif()

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/phonecam
    FILES_MATCHING PATTERN "*.h"
)