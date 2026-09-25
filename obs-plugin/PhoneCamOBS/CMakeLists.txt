cmake_minimum_required(VERSION 3.24)
project(PhoneCamOBS LANGUAGES CXX)
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

# OBS SDK - try to find it
# OBS typically installs cmake config in lib/obs/cmake or similar
find_path(OBS_INCLUDE_DIR obs-module.h
    PATHS
    "C:/Program Files/obs-studio/include"
    "C:/Program Files (x86)/obs-studio/include"
    "/usr/include/obs"
    "/usr/local/include/obs"
    "${CMAKE_SOURCE_DIR}/../../obs-studio/libobs"
    "${CMAKE_SOURCE_DIR}/../obs-studio/libobs"
    DOC "OBS Studio include directory"
)

find_library(OBS_LIBRARY obs
    PATHS
    "C:/Program Files/obs-studio/lib"
    "C:/Program Files (x86)/obs-studio/lib"
    "/usr/lib"
    "/usr/local/lib"
    "${CMAKE_SOURCE_DIR}/../../obs-studio/build/libobs"
    "${CMAKE_SOURCE_DIR}/../obs-studio/build/libobs"
    DOC "OBS Studio library"
)

if(NOT OBS_INCLUDE_DIR OR NOT OBS_LIBRARY)
    message(WARNING "OBS Studio SDK not found. Plugin will not build correctly.")
    message(WARNING "Set OBS_INCLUDE_DIR and OBS_LIBRARY manually or install OBS Studio SDK.")
endif()

add_library(ipc_client src/ipc_client.cpp)
target_include_directories(ipc_client PRIVATE include)
target_link_libraries(ipc_client PRIVATE OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)

add_library(phonecam_obs MODULE src/phonecam_source.cpp)
target_include_directories(phonecam_obs PRIVATE include)
target_link_libraries(phonecam_obs PRIVATE ipc_client)

if(OBS_INCLUDE_DIR AND OBS_LIBRARY)
    target_include_directories(phonecam_obs PRIVATE ${OBS_INCLUDE_DIR})
    target_link_libraries(phonecam_obs PRIVATE ${OBS_LIBRARY})
endif()

# Platform-specific settings
if(WIN32)
    target_compile_definitions(phonecam_obs PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(phonecam_obs PRIVATE ws2_32)
    target_compile_definitions(ipc_client PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(ipc_client PRIVATE ws2_32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(phonecam_obs PRIVATE pthread dl rt)
    target_link_libraries(ipc_client PRIVATE pthread dl rt)
elseif(APPLE)
    target_link_libraries(phonecam_obs PRIVATE pthread)
    target_link_libraries(ipc_client PRIVATE pthread)
endif()

# Security hardening flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(phonecam_obs PRIVATE
        -Wall -Wextra -Wpedantic
        -Wformat=2 -Wformat-security
        -Wstack-protector-strong
        -fstack-protector-strong
        -D_FORTIFY_SOURCE=2
    )
    target_link_options(phonecam_obs PRIVATE
        -Wl,-z,relro,-z,now
    )
    target_compile_options(ipc_client PRIVATE
        -Wall -Wextra -Wpedantic
        -Wformat=2 -Wformat-security
        -Wstack-protector-strong
        -fstack-protector-strong
        -D_FORTIFY_SOURCE=2
    )
    target_link_options(ipc_client PRIVATE
        -Wl,-z,relro,-z,now
    )
elseif(MSVC)
    target_compile_options(phonecam_obs PRIVATE
        /W4 /WX
        /sdl
        /guard:cf
    )
    target_link_options(phonecam_obs PRIVATE
        /DYNAMICBASE /NXCOMPAT
    )
    target_compile_options(ipc_client PRIVATE
        /W4 /WX
        /sdl
        /guard:cf
    )
    target_link_options(ipc_client PRIVATE
        /DYNAMICBASE /NXCOMPAT
    )
endif()
