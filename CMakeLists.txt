cmake_minimum_required(VERSION 3.16)
project(recomp_net VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(RNET_ENABLE_ICE "Build libjuice ICE transport" OFF)
option(RNET_ICE_BUNDLE_STATIC
  "Fetch/build static libjuice into recomp_net (portable; no system .so)" ON)
option(RNET_ICE_FORCE_TURN
  "Testing: only emit/accept ICE typ relay candidates (requires TURN)" OFF)
option(RNET_BUILD_EXAMPLES "Build example programs" ON)
option(RNET_BUILD_TESTS "Build unit tests" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(RNET_CORE_SOURCES
  src/platform/rnet_address.c
  src/platform/rnet_platform.c
  src/platform/rnet_stun.c
  src/protocol/rnet_protocol.c
  src/input/rnet_rings.c
  src/lobby/rnet_lan_lobby.c
  src/lobby/rnet_lan_direct.c
  src/transport/rnet_transport.c
  src/session/rnet_session.c
  src/ice/rnet_ice_state.c
  src/ice/rnet_ice_agent.c
)

add_library(recomp_net STATIC ${RNET_CORE_SOURCES})
add_library(recomp_net::recomp_net ALIAS recomp_net)

target_include_directories(recomp_net
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(recomp_net PRIVATE $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>)
if(UNIX)
  target_compile_definitions(recomp_net PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
  # Darwin gates BSD extensions in <net/if.h> behind
  # `!_POSIX_C_SOURCE || _DARWIN_C_SOURCE`. Strict POSIX above hides IFF_UP
  # (rnet_address.c). _DEFAULT_SOURCE is glibc-only and does nothing here.
  if(APPLE)
    target_compile_definitions(recomp_net PRIVATE _DARWIN_C_SOURCE)
  endif()
endif()

if(WIN32)
  target_link_libraries(recomp_net PUBLIC ws2_32 iphlpapi bcrypt)
endif()

if(RNET_ENABLE_ICE)
  # Portable game embeds: prefer a static juice linked into recomp_net so Linux
  # binaries do not depend on a distro libjuice.so at runtime. Opt out with
  # -DRNET_ICE_BUNDLE_STATIC=OFF to use find_package(Libjuice) (shared OK).
  set(_rnet_juice_ok FALSE)
  if(NOT RNET_ICE_BUNDLE_STATIC)
    find_package(Libjuice QUIET)
    if(Libjuice_FOUND)
      set(_rnet_juice_ok TRUE)
    endif()
  endif()
  if(NOT _rnet_juice_ok)
    include(FetchContent)
    message(STATUS
      "Libjuice: bundling static juice via FetchContent "
      "(RNET_ICE_BUNDLE_STATIC=${RNET_ICE_BUNDLE_STATIC})")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    set(NO_SERVER ON CACHE BOOL "" FORCE)
    set(USE_NETTLE OFF CACHE BOOL "" FORCE)
    set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      libjuice
      GIT_REPOSITORY https://github.com/paullouisageneau/libjuice.git
      GIT_TAG        v1.7.2
      GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(libjuice)
    # Parent -std=c11 / strict ISO hides getaddrinfo without a feature macro.
    if(TARGET juice-static)
      target_compile_definitions(juice-static PRIVATE _GNU_SOURCE)
    endif()
    if(TARGET juice)
      target_compile_definitions(juice PRIVATE _GNU_SOURCE)
    endif()
    if(NOT TARGET Libjuice::Libjuice)
      if(TARGET juice-static)
        add_library(Libjuice::Libjuice ALIAS juice-static)
      elseif(TARGET juice)
        add_library(Libjuice::Libjuice ALIAS juice)
      else()
        message(FATAL_ERROR
          "RNET_ENABLE_ICE=ON but libjuice was not found and FetchContent did not "
          "produce a juice target. Install libjuice, set RNET_LIBJUICE_ROOT, "
          "vendor sources under third_party/libjuice, or enable network for "
          "FetchContent.")
      endif()
    endif()
  endif()
  target_compile_definitions(recomp_net PUBLIC RNET_ENABLE_ICE=1)
  if(RNET_ICE_FORCE_TURN)
    target_compile_definitions(recomp_net PUBLIC RNET_ICE_FORCE_TURN=1)
    message(STATUS "recomp_net: RNET_ICE_FORCE_TURN — relay-only ICE candidates")
  endif()
  target_link_libraries(recomp_net PUBLIC Libjuice::Libjuice)
  if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(recomp_net PUBLIC Threads::Threads)
  endif()
  # When ICE is off, session uses inline stubs from rnet_ice_internal.h.
endif()

if(MSVC)
  target_compile_options(recomp_net PRIVATE /W4)
else()
  target_compile_options(recomp_net PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- Examples ----
if(RNET_BUILD_EXAMPLES)
  add_executable(lan_delay_2p examples/lan_delay_2p/main.c)
  target_link_libraries(lan_delay_2p PRIVATE recomp_net)

  add_executable(ice_manual_signaling examples/ice_manual_signaling/main.c)
  target_link_libraries(ice_manual_signaling PRIVATE recomp_net)
endif()

# ---- Tests ----
if(RNET_BUILD_TESTS)
  enable_testing()

  add_executable(ring_admit_test tests/ring_admit_test.c)
  target_link_libraries(ring_admit_test PRIVATE recomp_net)
  target_include_directories(ring_admit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME ring_admit_test COMMAND ring_admit_test)

  add_executable(wire_map_test tests/wire_map_test.c)
  target_link_libraries(wire_map_test PRIVATE recomp_net)
  add_test(NAME wire_map_test COMMAND wire_map_test)

  add_executable(hostport_test tests/hostport_test.c)
  target_link_libraries(hostport_test PRIVATE recomp_net)
  target_include_directories(hostport_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME hostport_test COMMAND hostport_test)

  add_executable(lan_lobby_test tests/lan_lobby_test.c)
  target_link_libraries(lan_lobby_test PRIVATE recomp_net)
  add_test(NAME lan_lobby_test COMMAND lan_lobby_test)

  add_executable(udp_port_test tests/udp_port_test.c)
  target_link_libraries(udp_port_test PRIVATE recomp_net)
  target_include_directories(udp_port_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME udp_port_test COMMAND udp_port_test)

  add_executable(address_test tests/address_test.c)
  target_link_libraries(address_test PRIVATE recomp_net)
  target_include_directories(address_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME address_test COMMAND address_test)

  add_executable(stun_test tests/stun_test.c)
  target_link_libraries(stun_test PRIVATE recomp_net)
  target_include_directories(stun_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME stun_test COMMAND stun_test)

  add_executable(session_state_test tests/session_state_test.c)
  target_link_libraries(session_state_test PRIVATE recomp_net)
  add_test(NAME session_state_test COMMAND session_state_test)
endif()

install(TARGETS recomp_net ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(DIRECTORY include/recomp_net DESTINATION include)
