# PS5 Remote Play for Veyra on macOS (and the Linux test build).
#
# Source: chiaki-ng pinned by Veyra 2.1.9 (0e16950165f06e5c3291537c2eeba6e852be7120)
# plus Veyra's reviewed patches (third_party/chiaki-patches), prepared by
#   scripts/fetch-chiaki.sh <dir>   and passed as -DVEYRA_CHIAKI_SOURCE_DIR=<dir>.
#
# Dependencies and how they are found (works with system packages and vcpkg):
#   OpenSSL (libcrypto)  find_package(OpenSSL)            — chiaki's own FindOpenSSL wraps it
#   Opus                 find_package(Opus)               — chiaki's cmake/FindOpus.cmake (pkg-config / find_library)
#   json-c               pkg-config  json-c               — chiaki lib/CMakeLists (pkg_search_module)
#   miniupnpc            pkg-config  miniupnpc            — chiaki lib/CMakeLists
#   libevent             pkg-config  libevent, else find_package(libevent)
#   curl                 bundled submodule (third-party/curl, WebSocket build), uses OpenSSL
#   nanopb, jerasure,    bundled submodules
#   gf-complete
#   SDL3                 find_package(SDL3 CONFIG)        — gamepad input (DualSense/DualShock/Xbox)
#   FFmpeg               find_package(FFMPEG) (vcpkg wrapper: FFMPEG_INCLUDE_DIRS/FFMPEG_LIBRARIES),
#                        else pkg-config libavcodec libavutil libswscale (Linux apt) — VideoToolbox decode on macOS
# Build tools: Python 3 with the `protobuf` module (nanopb generator; pass Python3_EXECUTABLE /
# PYTHON_EXECUTABLE for a venv), pkg-config.
include_guard(GLOBAL)
enable_language(C)
if(NOT EXISTS "${VEYRA_CHIAKI_SOURCE_DIR}/lib/include/chiaki/session.h")
  message(FATAL_ERROR "VEYRA_ENABLE_REMOTEPLAY needs -DVEYRA_CHIAKI_SOURCE_DIR=<dir prepared by scripts/fetch-chiaki.sh>")
endif()
file(READ "${VEYRA_CHIAKI_SOURCE_DIR}/lib/include/chiaki/session.h" _veyra_chiaki_session)
if(NOT _veyra_chiaki_session MATCHES "CHIAKI_VEYRA_VIDEO_METADATA_API")
  message(FATAL_ERROR "chiaki-ng at ${VEYRA_CHIAKI_SOURCE_DIR} lacks Veyra's patches; run scripts/fetch-chiaki.sh")
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(NOT PYTHON_EXECUTABLE)
  set(PYTHON_EXECUTABLE "${Python3_EXECUTABLE}" CACHE FILEPATH "" FORCE)
endif()
set(CHIAKI_LIB_JSONC_EXTERNAL_PROJECT OFF CACHE BOOL "" FORCE)
set(CHIAKI_LIB_MINIUPNPC_EXTERNAL_PROJECT OFF CACHE BOOL "" FORCE)

set(CHIAKI_ENABLE_GUI OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_CLI OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_ANDROID OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_BOREALIS OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_FFMPEG_DECODER OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_PI_DECODER OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_SETSU OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_STEAMDECK_NATIVE OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_STEAM_SHORTCUT OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_SPEEX OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_RUDP OFF CACHE STRING "" FORCE)
set(CHIAKI_LIB_ENABLE_OPUS ON CACHE BOOL "" FORCE)
set(CHIAKI_LIB_ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(CHIAKI_USE_SYSTEM_NANOPB OFF CACHE STRING "" FORCE)
set(CHIAKI_USE_SYSTEM_JERASURE OFF CACHE STRING "" FORCE)
set(CHIAKI_USE_SYSTEM_CURL OFF CACHE STRING "" FORCE)
# Bundled curl: only HTTP(S)/WS over OpenSSL; no optional system libraries.
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

add_subdirectory("${VEYRA_CHIAKI_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/chiaki" EXCLUDE_FROM_ALL)
if(NOT TARGET chiaki-lib)
  message(FATAL_ERROR "chiaki-ng did not create chiaki-lib")
endif()

find_package(SDL3 CONFIG REQUIRED)
find_package(FFMPEG QUIET)
if(FFMPEG_FOUND AND FFMPEG_LIBRARIES)
  add_library(veyra_ffmpeg INTERFACE)
  target_include_directories(veyra_ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(veyra_ffmpeg INTERFACE ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(veyra_ffmpeg INTERFACE ${FFMPEG_LIBRARIES})
else()
  pkg_check_modules(VEYRA_FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
  add_library(veyra_ffmpeg INTERFACE)
  target_link_libraries(veyra_ffmpeg INTERFACE PkgConfig::VEYRA_FFMPEG)
endif()

add_library(veyra_remoteplay STATIC
  src/remoteplay/Validation.cpp
  src/remoteplay/AnnexB.cpp
  src/remoteplay/Timeline.cpp
  src/remoteplay/VideoIngress.cpp
  src/remoteplay/SessionInbox.cpp
  src/remoteplay/ChiakiBackend.cpp
  src/remoteplay/ChiakiRegistration.cpp
  src/remoteplay/Discovery.cpp
  src/remoteplay/ProfileStore.cpp
  src/remoteplay/ControllerInput.cpp
  src/remoteplay/RemotePlaySession.cpp
  src/remoteplay/RemotePlaySession.h)
target_include_directories(veyra_remoteplay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(veyra_remoteplay PUBLIC cxx_std_20)
if(TARGET SDL3::SDL3)
  set(_veyra_sdl SDL3::SDL3)
else()
  set(_veyra_sdl SDL3::SDL3-static)
endif()
target_link_libraries(veyra_remoteplay
  PUBLIC Qt6::Core Qt6::Multimedia
  PRIVATE chiaki-lib ${_veyra_sdl} veyra_ffmpeg Threads::Threads)
if(APPLE)
  target_link_libraries(veyra_remoteplay PRIVATE "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo"
    "-framework CoreFoundation" "-framework Security")
endif()
