#
# Loads libdatachannel for the Moonlight WebRTC TV stream, giving the priority to the system package first,
# with a fallback to FetchContent. MSYS2 does not package libdatachannel, so Windows builds use the fallback.
#
include_guard(GLOBAL)

set(SUNSHINE_LIBDATACHANNEL_VERSION "v0.24.6" CACHE STRING "libdatachannel release built when no system package exists")

find_package(LibDataChannel 0.24 CONFIG QUIET)
if(LibDataChannel_FOUND)
    if(TARGET LibDataChannel::LibDataChannelStatic)
        set(SUNSHINE_LIBDATACHANNEL_TARGET LibDataChannel::LibDataChannelStatic)
    else()
        set(SUNSHINE_LIBDATACHANNEL_TARGET LibDataChannel::LibDataChannel)
    endif()
    message(STATUS "Using system libdatachannel ${LibDataChannel_VERSION}")
    return()
endif()

message(STATUS "libdatachannel package not found in the system. Falling back to FetchContent.")
include(FetchContent)

# Media transport (SRTP) and the signaling WebSocket server are required; everything else is trimmed.
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(USE_NICE OFF CACHE BOOL "" FORCE)
# Sunshine already provides nlohmann_json; the bundled copy would define the same target.
set(USE_SYSTEM_JSON ON CACHE BOOL "" FORCE)
# libSRTP treats its MinGW printf-format warnings as errors and builds test programs by default.
set(ENABLE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG ${SUNSHINE_LIBDATACHANNEL_VERSION}
        GIT_SHALLOW TRUE
        GIT_SUBMODULES deps/plog deps/usrsctp deps/libjuice deps/libsrtp
        EXCLUDE_FROM_ALL
)

# Build the static library only, without touching Sunshine's own BUILD_SHARED_LIBS choice.
set(_sunshine_build_shared_libs "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(libdatachannel)
set(BUILD_SHARED_LIBS "${_sunshine_build_shared_libs}")
unset(_sunshine_build_shared_libs)

set(SUNSHINE_LIBDATACHANNEL_TARGET LibDataChannel::LibDataChannelStatic)
if(NOT TARGET ${SUNSHINE_LIBDATACHANNEL_TARGET})
    set(SUNSHINE_LIBDATACHANNEL_TARGET datachannel-static)
endif()
