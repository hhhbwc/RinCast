# rincast-core: protocol, transport, crypto and audio codec shared by the
# PC streamer and the headset app.
cmake_minimum_required(VERSION 3.22)

set(RC_THIRD_PARTY ${CMAKE_CURRENT_LIST_DIR}/../third_party)

set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
if(NOT TARGET opus)
  set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  if(MSVC AND DEFINED CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
    set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
  endif()
  add_subdirectory(${RC_THIRD_PARTY}/opus ${CMAKE_CURRENT_BINARY_DIR}/opus EXCLUDE_FROM_ALL)
endif()

add_library(rincast-core STATIC
  src/platform.cpp
  src/socket.cpp
  src/crypto.cpp
  src/connection.cpp
  src/handshake.cpp
  src/video_transport.cpp
  src/audio_codec.cpp
  src/music.cpp
  src/discovery.cpp
  ${RC_THIRD_PARTY}/monocypher/monocypher.c
)
target_include_directories(rincast-core PUBLIC
  include
  ${RC_THIRD_PARTY}
  ${RC_THIRD_PARTY}/monocypher
)
target_compile_features(rincast-core PUBLIC cxx_std_20)
target_link_libraries(rincast-core PUBLIC opus)
if(WIN32)
  target_compile_definitions(rincast-core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  target_link_libraries(rincast-core PUBLIC ws2_32 iphlpapi bcrypt)
elseif(ANDROID)
  target_link_libraries(rincast-core PUBLIC log)
endif()

option(RINCAST_CORE_TESTS "Build rincast-core tests" OFF)
if(RINCAST_CORE_TESTS)
  add_executable(rincast-core-tests tests/core_tests.cpp)
  target_link_libraries(rincast-core-tests PRIVATE rincast-core)
endif()

# ROUND19 T2: CPU-only hardening options, all default off (release builds unaffected).
#   RINCAST_SANITIZE=address -> MSVC /fsanitize=address on rincast-core and the test
#   binaries. MSVC has no UBSan equivalent; use RINCAST_ANALYZE=ON for /analyze instead.
#   RINCAST_FUZZ=ON          -> the deterministic fuzz drivers in tests/fuzz_drivers.cpp.
set(RINCAST_SANITIZE "" CACHE STRING "Sanitizer: \"address\" (MSVC /fsanitize=address) or empty")
option(RINCAST_ANALYZE "Add /analyze to rincast-core sources (MSVC)" OFF)
option(RINCAST_FUZZ "Build the deterministic fuzz drivers" OFF)

set(RC_HARDEN_FLAGS "")
if(RINCAST_SANITIZE)
  if(MSVC AND RINCAST_SANITIZE STREQUAL "address")
    set(RC_HARDEN_FLAGS /fsanitize=address)
    # /RTC (CMake default in Debug) is incompatible with /fsanitize=address.
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
  else()
    message(WARNING "RINCAST_SANITIZE='${RINCAST_SANITIZE}' is not supported by this toolchain; ignored")
  endif()
endif()
if(RINCAST_ANALYZE AND MSVC)
  list(APPEND RC_HARDEN_FLAGS /analyze)
endif()

function(rc_harden target)
  if(RC_HARDEN_FLAGS)
    target_compile_options(${target} PRIVATE ${RC_HARDEN_FLAGS})
    target_link_options(${target} PRIVATE ${RC_HARDEN_FLAGS})
  endif()
endfunction()

rc_harden(rincast-core)
if(TARGET rincast-core-tests)
  rc_harden(rincast-core-tests)
endif()
if(RINCAST_FUZZ)
  add_executable(rincast-fuzz tests/fuzz_drivers.cpp)
  target_link_libraries(rincast-fuzz PRIVATE rincast-core)
  # ROUND19 T2: the fuzz target is the ONLY place the reassembly test seam is compiled
  # in; default builds (streamer, core-tests) never see it.
  target_compile_definitions(rincast-fuzz PRIVATE RINCAST_TEST_EXPOSE_REASSEMBLY)
  rc_harden(rincast-fuzz)
endif()
