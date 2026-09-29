cmake_minimum_required(VERSION 3.24)
# Allow CMake 3.5+ policies for older dependencies (like plog in libdatachannel)
# Set policy version minimum to 3.5 to allow subprojects with older cmake_minimum_required
cmake_policy(VERSION 3.5)
cmake_policy(SET CMP0048 NEW)
cmake_policy(SET CMP0054 NEW)
cmake_policy(SET CMP0074 NEW)
cmake_policy(SET CMP0077 NEW)
cmake_policy(SET CMP0078 NEW)
cmake_policy(SET CMP0094 NEW)
cmake_policy(SET CMP0102 NEW)
cmake_policy(SET CMP0104 NEW)
cmake_policy(SET CMP0106 NEW)
cmake_policy(SET CMP0118 NEW)
cmake_policy(SET CMP0126 NEW)
project(PhoneCamDesktop LANGUAGES CXX)
cmake_policy(SET CMP0158 NEW)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenSSL (system-installed). OPENSSL_ROOT_DIR hint must be passed by caller
# (e.g. -DOPENSSL_ROOT_DIR="C:/Program Files/OpenSSL-Win64"). We use the shared
# DLL runtime shipped with OpenSSL-Win64.
find_package(OpenSSL REQUIRED)

include(FetchContent)

# Find ZLIB (required by ixwebsocket) - try system first, then FetchContent
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    # Use FetchContent for ZLIB if not found on system
    FetchContent_Declare(
        zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(zlib)
    # Make ZLIB available as a package
    # zlib generates zconf.h in the build directory, so we need both source and binary dirs
    set(ZLIB_FOUND TRUE)
    set(ZLIB_INCLUDE_DIRS ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
    # Create an alias for the zlibstatic target so it can be used globally
    if(TARGET zlibstatic AND NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()
    set(ZLIB_LIBRARIES ZLIB::ZLIB)
endif()

# nlohmann/json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ixwebsocket: WebSocket client library for signaling to Android
# Need to set ZLIB_ROOT so ixwebsocket can find it
# zconf.h is generated in the build directory, so we need both source and binary dirs
set(ZLIB_ROOT ${zlib_SOURCE_DIR})
set(ZLIB_INCLUDE_DIR ${zlib_SOURCE_DIR})
set(ZLIB_INCLUDE_DIRS ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
set(ZLIB_LIBRARY zlibstatic)

FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v12.0.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ixwebsocket)

# Ensure zlib is built before ixwebsocket so zconf.h is available
add_dependencies(ixwebsocket zlibstatic)

# Add zlib binary dir to ixwebsocket include paths so it can find zconf.h
target_include_directories(ixwebsocket PRIVATE ${zlib_BINARY_DIR})

# ---------------------------------------------------------------------------
# libdatachannel: WebRTC DataChannel + Media transport.
#
# - USE_GNUTLS / USE_MBEDTLS are OFF so OpenSSL is used (matches system OpenSSL).
# - NO_EXAMPLES / NO_TESTS to keep the build lean.
# - NO_MEDIA=OFF enables RTP/RTCP media transport (required for WebRTC tracks).
# - USE_NICE=OFF avoids libnice dep; we use libjuice for ICE.
# - PREFER_SYSTEM_LIB=OFF lets it build usrsctp + libjuice + libsrtp-internal
#   from their own submodules via FetchContent.
# - We expose datachannel-static if it was created.
# ---------------------------------------------------------------------------
set(NO_EXAMPLES ON  CACHE BOOL "" FORCE)
set(NO_TESTS    ON  CACHE BOOL "" FORCE)
set(NO_MEDIA    OFF CACHE BOOL "" FORCE)
set(USE_NICE    OFF CACHE BOOL "" FORCE)
set(USE_GNUTLS  OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(PREFER_SYSTEM_LIB OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.23.1
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(libdatachannel)

# libdatachannel produces target "datachannel" (shared) or "datachannel-static"
# (static). Prefer the static target when present so we do not ship extra DLLs.
if(TARGET datachannel-static)
    set(DATACHANNEL_TARGET datachannel-static)
elseif(TARGET datachannel)
    set(DATACHANNEL_TARGET datachannel)
else()
    message(FATAL_ERROR "libdatachannel did not produce a usable target (datachannel or datachannel-static)")
endif()
message(STATUS "Using libdatachannel target: ${DATACHANNEL_TARGET}")

# ---------------------------------------------------------------------------
# libvpx: needed for VP8 -> I420 decode. Upstream libvpx has no CMake support
# (autotools + Perl+configure only). Perl is NOT installed on this host, so we
# cannot build libvpx from source.
#
# Strategy:
#   - Look for a system / vcpkg-installed libvpx (VPX_ROOT hint, or system include+lib).
#   - If not found: build in "depay-only" mode; WebRtcReceiver emits VP8
#     bitstream frames and the preview window shows connection/frame counters
#     and the most recent frame byte size (real data) without decoding pixels.
# ---------------------------------------------------------------------------
option(PHONECAM_ENABLE_VPX_DECODE "Enable libvpx VP8 decode pipeline" ON)
set(VPX_ROOT "" CACHE PATH "Path to a prebuilt libvpx installation (include/ and lib/)")

set(VPX_FOUND FALSE)
if(PHONECAM_ENABLE_VPX_DECODE)
    if(VPX_ROOT)
        find_path(VPX_INCLUDE_DIR vpx/vpx_decoder.h PATHS "${VPX_ROOT}/include" NO_DEFAULT_PATH)
        find_library(VPX_LIBRARY NAMES vpx vpxmd PATHS "${VPX_ROOT}/lib" NO_DEFAULT_PATH)
        if(VPX_INCLUDE_DIR AND VPX_LIBRARY)
            set(VPX_FOUND TRUE)
            message(STATUS "Found VPX at ${VPX_ROOT}")
        endif()
    endif()

    if(NOT VPX_FOUND)
        # Try vcpkg / system paths
        find_path(VPX_INCLUDE_DIR vpx/vpx_decoder.h)
        find_library(VPX_LIBRARY NAMES vpx vpxmd)
        if(VPX_INCLUDE_DIR AND VPX_LIBRARY)
            set(VPX_FOUND TRUE)
            message(STATUS "Found VPX in system paths: ${VPX_LIBRARY}")
        endif()
    endif()
endif()

if(NOT VPX_FOUND)
    message(WARNING "libvpx not found. Building WebRtcReceiver in DEPAY-ONLY mode:")
    message(WARNING "  - VP8 frames are assembled from RTP but not decoded to I420.")
    message(WARNING "  - Preview window will show real connection state and frame counters.")
    message(WARNING "  - To enable pixel decoding, install vcpkg and 'vcpkg install libvpx:x64-windows',")
    message(WARNING "    then re-run CMake with -DVPX_ROOT=<vcpkg>/installed/x64-windows")
    set(PHONECAM_DEPAY_ONLY TRUE)
else()
    set(PHONECAM_DEPAY_ONLY FALSE)
endif()

# ---------------------------------------------------------------------------
# libopus: needed for Opus -> PCM decode.
#
# Strategy:
#   - Look for a system / vcpkg-installed libopus (OPUS_ROOT hint, or system include+lib).
#   - If not found: WebRtcReceiver emits Opus bitstream frames and consumers
#     can decode externally or pass-through to OBS.
# ---------------------------------------------------------------------------
option(PHONECAM_ENABLE_OPUS_DECODE "Enable libopus Opus decode pipeline" ON)
set(OPUS_ROOT "" CACHE PATH "Path to a prebuilt libopus installation (include/ and lib/)")

set(OPUS_FOUND FALSE)
if(PHONECAM_ENABLE_OPUS_DECODE)
    if(OPUS_ROOT)
        find_path(OPUS_INCLUDE_DIR opus/opus.h PATHS "${OPUS_ROOT}/include" NO_DEFAULT_PATH)
        find_library(OPUS_LIBRARY NAMES opus opuslib PATHS "${OPUS_ROOT}/lib" NO_DEFAULT_PATH)
        if(OPUS_INCLUDE_DIR AND OPUS_LIBRARY)
            set(OPUS_FOUND TRUE)
            message(STATUS "Found OPUS at ${OPUS_ROOT}")
        endif()
    endif()

    if(NOT OPUS_FOUND)
        # Try vcpkg / system paths
        find_path(OPUS_INCLUDE_DIR opus/opus.h)
        find_library(OPUS_LIBRARY NAMES opus opuslib)
        if(OPUS_INCLUDE_DIR AND OPUS_LIBRARY)
            set(OPUS_FOUND TRUE)
            message(STATUS "Found OPUS in system paths: ${OPUS_LIBRARY}")
        endif()
    endif()
endif()

if(NOT OPUS_FOUND)
    message(WARNING "libopus not found. Building WebRtcReceiver in Opus DEPAY-ONLY mode:")
    message(WARNING "  - Opus frames are assembled from RTP but not decoded to PCM.")
    message(WARNING "  - Consumers will receive raw Opus bitstream frames.")
    message(WARNING "  - To enable PCM decoding, install vcpkg and 'vcpkg install opus:x64-windows',")
    message(WARNING "    then re-run CMake with -DOPUS_ROOT=<vcpkg>/installed/x64-windows")
    set(PHONECAM_OPUS_DEPAY_ONLY TRUE)
else()
    set(PHONECAM_OPUS_DEPAY_ONLY FALSE)
endif()

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Common source list shared by PhoneCamDesktop and PhoneCamPreview.
# We define an OBJECT library so the two executables share the same core code
# without recompiling twice, and tests can also link against it.
# ---------------------------------------------------------------------------
set(PHONECAM_CORE_SOURCES
    src/core/SessionManager.cpp
    src/core/SecurityManager.cpp
    src/core/SecureStorage.cpp
    src/core/AudioDistributor.cpp
    src/core/IpcServer.cpp
    src/core/WebRtcReceiver.cpp
    src/core/SignalingServer.cpp
    src/core/DiscoveryClient.cpp
    src/core/SignalingClient.cpp
    src/media/Vp8Depayloader.cpp
    src/media/OpusDepayloader.cpp
)

# Vp8Decoder.cpp only compiles when libvpx is available; its body is fully
# guarded by #ifdef PHONECAM_HAVE_VPX so it is safe to always list it.
list(APPEND PHONECAM_CORE_SOURCES src/media/Vp8Decoder.cpp)

# OpusDecoder.cpp only compiles when libopus is available; its body is fully
# guarded by #ifdef PHONECAM_HAVE_OPUS so it is safe to always list it.
list(APPEND PHONECAM_CORE_SOURCES src/media/OpusDecoder.cpp)

# I420ToRgb24 is used by preview_main.cpp; it does NOT depend on libvpx.
list(APPEND PHONECAM_CORE_SOURCES src/media/I420ToRgb24.cpp)

add_library(phonecam_core STATIC ${PHONECAM_CORE_SOURCES})

target_include_directories(phonecam_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src         # so #include "media/Vp8Depayloader.h" etc. works
)

target_link_libraries(phonecam_core PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    ${DATACHANNEL_TARGET}
    ixwebsocket
    ZLIB::ZLIB
)

if(NOT PHONECAM_DEPAY_ONLY)
    target_include_directories(phonecam_core PUBLIC ${VPX_INCLUDE_DIR})
    target_link_libraries(phonecam_core PUBLIC ${VPX_LIBRARY})
    target_compile_definitions(phonecam_core PUBLIC PHONECAM_HAVE_VPX=1)
else()
    target_compile_definitions(phonecam_core PUBLIC PHONECAM_DEPAY_ONLY=1)
endif()

if(NOT PHONECAM_OPUS_DEPAY_ONLY)
    target_include_directories(phonecam_core PUBLIC ${OPUS_INCLUDE_DIR})
    target_link_libraries(phonecam_core PUBLIC ${OPUS_LIBRARY})
    target_compile_definitions(phonecam_core PUBLIC PHONECAM_HAVE_OPUS=1)
else()
    target_compile_definitions(phonecam_core PUBLIC PHONECAM_OPUS_DEPAY_ONLY=1)
endif()

if(WIN32)
    # NOTE: _CRT_SECURE_NO_WARNINGS is intentionally NOT defined globally because
    # some sources (e.g. SecureStorage.cpp) already define it locally; doing both
    # produces C4005 macro-redefinition which we treat as an error via /WX.
    target_compile_definitions(phonecam_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(phonecam_core PUBLIC ws2_32 iphlpapi bcrypt crypt32 dnsapi)
    # Add Windows SDK include directory for dnsapi.h and other Windows SDK headers
    if(CMAKE_SYSTEM_VERSION)
        # Try to find Windows SDK include path
        find_path(WINSDK_INCLUDE_DIR "dnsapi.h"
            PATHS
                "$ENV{WINDOWSSDKDIR}/Include"
                "$ENV{WINDOWSSDKDIR}/include"
                "C:/Program Files (x86)/Windows Kits/10/Include"
                "C:/Program Files/Windows Kits/10/Include"
            PATH_SUFFIXES "um" "shared"
            NO_DEFAULT_PATH)
        if(WINSDK_INCLUDE_DIR)
            target_include_directories(phonecam_core PUBLIC ${WINSDK_INCLUDE_DIR})
        endif()
    endif()
endif()

if(MSVC)
    target_compile_options(phonecam_core PRIVATE /W4 /WX /sdl /guard:cf /wd4324)
    target_link_options(phonecam_core PUBLIC /DYNAMICBASE /NXCOMPAT)
else()
    target_compile_options(phonecam_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# PhoneCamDesktop (existing console app; main.cpp)
# ---------------------------------------------------------------------------
add_executable(PhoneCamDesktop src/main.cpp)
target_link_libraries(PhoneCamDesktop PRIVATE phonecam_core)

# Copy datachannel.dll (when shared) next to the executable so the binary is
# runnable in-place without PATH manipulation. This is a no-op for static
# targets (no DLL produced).
if(TARGET datachannel)
    add_custom_command(TARGET PhoneCamDesktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:datachannel>"
            "$<TARGET_FILE_DIR:PhoneCamDesktop>"
        COMMENT "Staging datachannel.dll next to PhoneCamDesktop.exe"
    )
endif()

# ---------------------------------------------------------------------------
# PhoneCamPreview (new Win32 GDI preview app that shares the same core)
# ---------------------------------------------------------------------------
add_executable(PhoneCamPreview WIN32 src/preview_main.cpp)
target_link_libraries(PhoneCamPreview PRIVATE phonecam_core)
if(WIN32)
    target_link_libraries(PhoneCamPreview PRIVATE gdi32 user32)
endif()

if(TARGET datachannel)
    add_custom_command(TARGET PhoneCamPreview POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:datachannel>"
            "$<TARGET_FILE_DIR:PhoneCamPreview>"
        COMMENT "Staging datachannel.dll next to PhoneCamPreview.exe"
    )
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
add_executable(SecurityManagerTests tests/SecurityManagerTests.cpp)
target_link_libraries(SecurityManagerTests PRIVATE phonecam_core)

if(TARGET datachannel)
    add_custom_command(TARGET SecurityManagerTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:datachannel>"
            "$<TARGET_FILE_DIR:SecurityManagerTests>"
        COMMENT "Staging datachannel.dll next to SecurityManagerTests.exe"
    )
endif()

enable_testing()
add_test(NAME SecurityManagerTests COMMAND SecurityManagerTests)

# Install rules
install(TARGETS PhoneCamDesktop PhoneCamPreview
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
