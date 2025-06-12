cmake_minimum_required(VERSION 3.7.1)
project(cpp_streamer VERSION 2.5.0)

set(CMAKE_CXX_STANDARD 11)
find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
    message(STATUS "found library: ${OPENSSL_LIBRARIES}, ENABLE_OPENSSL defined")
    add_library(openssl INTERFACE)
    include_directories(${OPENSSL_INCLUDE_DIR})
    target_include_directories(openssl INTERFACE ${OPENSSL_INCLUDE_DIR})
    target_link_libraries(openssl INTERFACE ${OPENSSL_LIBRARIES})
    if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND OPENSSL_USE_STATIC_LIBS)
        target_link_libraries(openssl INTERFACE ${CMAKE_DL_LIBS})
    elseif(CMAKE_SYSTEM_NAME MATCHES "Windows" AND OPENSSL_USE_STATIC_LIBS)
        target_link_libraries(openssl INTERFACE Crypt32)
    endif()
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
add_library(getopt STATIC src/utils/getopt.c)
list(APPEND LINK_LIBS getopt)
add_definitions(-DSTREAMER_EXPORT)
else()
#set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -std=c++11 -g -Wno-deprecated -Wno-deprecated-declarations -Wno-reorder -Wall -fexceptions -frtti -D__STDC_FORMAT_MACROS -fPIC")
set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -std=c++11 -g -Wno-deprecated -Wno-deprecated-declarations -Wall -fexceptions -frtti -D__STDC_FORMAT_MACROS -fPIC")
list(APPEND LINK_LIBS pthread dl z m)
if(LINUX)
list(APPEND LINK_LIBS rt)
endif()
endif()

set(CMAKE_OUTPUT_BASE ${CMAKE_BINARY_DIR}/output)
set(BUILD_OUTPUT_BASE ${CMAKE_BINARY_DIR}/output)
set(PREFIX_DIR "${BUILD_OUTPUT_BASE}")
set(INSTALL_RPATH "${PREFIX_DIR}/lib")

SET(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)
SET(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)

# set output static libary
# SET(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR}/output/lib)

# set pkgconfig path
set(CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}/output/lib/pkgconfig")

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
if (CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
message("-----This is MacOs arm64")
else()
message("-----This is MacOs Intel")
endif()
else()
message("-----This is system ${CMAKE_SYSTEM_NAME}")
endif()
message("")

# set include path
include_directories(${CMAKE_BINARY_DIR}/output/include
                    src
                    src/format
                    src/format/flv
                    src/format/mp4
                    src/format/mpegts
                    src/format/amf
                    src/format/sdp
                    src/format/ogg
                    src/net
                    src/net/rtprtcp
                    src/net/http
                    src/net/tcp
                    src/net/udp
                    src/net/stun
                    src/net/rtmp
                    src/net/webrtc
                    src/net/timesync
                    src/utils
                    src/utils/av
                    3rdparty/libuv/include
                    3rdparty/libsrtp/include
                    /usr/local/include)

# set lib path
IF (APPLE)
link_directories(${CMAKE_BINARY_DIR}/output/lib /usr/local/lib)
ELSEIF (UNIX)
link_directories(${CMAKE_BINARY_DIR}/output/lib /usr/local/lib64 /usr/local/lib)
ENDIF ()


add_subdirectory(3rdparty)

################################################################
## flvdemux streamer module
add_library(flvdemux SHARED
            ./src/format/audio_header.cpp
            ./src/format/h264_h265_header.cpp
            ./src/format/flv/flv_demux.cpp)
target_link_libraries(flvdemux ${LINK_LIBS})

################################################################
## flvmux streamer module
add_library(flvmux SHARED
            ./src/format/flv/flv_mux.cpp)
target_link_libraries(flvmux ${LINK_LIBS})

################################################################
## mpegtsmux streamer module
add_library(mpegtsmux SHARED
            ./src/format/audio_header.cpp
            ./src/format/h264_h265_header.cpp
            ./src/format/mpegts/mpegts_mux.cpp)
target_link_libraries(mpegtsmux ${LINK_LIBS})

################################################################
## mpegtsdemux streamer module
add_library(mpegtsdemux SHARED
            ./src/format/audio_header.cpp
            ./src/format/h264_h265_header.cpp
            ./src/format/opus_header.cpp
            ./src/format/mpegts/mpegts_demux.cpp)
target_link_libraries(mpegtsdemux ${LINK_LIBS})

################################################################
## mp4demux streamer module
add_library(mp4demux SHARED
            ./src/format/audio_header.cpp
            ./src/format/h264_h265_header.cpp
            ./src/format/opus_header.cpp
            ./src/format/mp4/mp4_demux.cpp)
target_link_libraries(mp4demux ${LINK_LIBS})

################################################################
## timesync streamer module
add_library(timesync SHARED
            ./src/net/timesync/timesync.cpp)
target_link_libraries(timesync ${LINK_LIBS})

################################################################
add_library(rtmp STATIC
             ./src/format/h264_h265_header.hpp
             ./src/format/h264_h265_header.cpp
            ./src/net/rtmp/chunk_stream.hpp
            ./src/net/rtmp/chunk_stream.cpp
            ./src/net/rtmp/rtmp_client_session.hpp
            ./src/net/rtmp/rtmp_client_session.cpp
            ./src/net/rtmp/rtmp_control_handler.hpp
            ./src/net/rtmp/rtmp_control_handler.cpp
            ./src/net/rtmp/rtmp_handshake.hpp
            ./src/net/rtmp/rtmp_handshake.cpp
            ./src/net/rtmp/rtmp_session_base.hpp
            ./src/net/rtmp/rtmp_session_base.cpp)
target_link_libraries(rtmp openssl uv ${LINK_LIBS})
################################################################
## rtmpplay streamer module
add_library(rtmpplay SHARED ./src/net/rtmp/rtmp_play.cpp)
target_link_libraries(rtmpplay rtmp)

################################################################
## rtmppublish streamer module
add_library(rtmppublish SHARED ./src/net/rtmp/rtmp_publish.cpp)
target_link_libraries(rtmppublish rtmp)

################################################################
add_library(rtc STATIC 
            ./src/net/webrtc/peerconnection.hpp
            ./src/net/webrtc/peerconnection.cpp
            ./src/net/webrtc/srtp_session.hpp
            ./src/net/webrtc/srtp_session.cpp
            ./src/net/webrtc/dtls.hpp
            ./src/net/webrtc/dtls.cpp
            ./src/net/webrtc/rtc_send_stream.hpp
            ./src/net/webrtc/rtc_send_stream.cpp
            ./src/net/webrtc/rtc_recv_stream.hpp
            ./src/net/webrtc/rtc_recv_stream.cpp
            ./src/net/webrtc/jitterbuffer.hpp
            ./src/net/webrtc/jitterbuffer.cpp
            ./src/net/webrtc/nack_generator.hpp
            ./src/net/webrtc/nack_generator.cpp
            ./src/net/webrtc/pack_handle_h264.hpp
            ./src/net/webrtc/pack_handle_h264.cpp
            ./src/net/http/http_client.hpp
            ./src/net/http/http_client.cpp
            ./src/net/rtprtcp/rtp_packet.hpp
            ./src/net/rtprtcp/rtp_packet.cpp
            ./src/net/rtprtcp/rtp_h264_pack.hpp
            ./src/net/rtprtcp/rtp_h264_pack.cpp
            ./src/format/sdp/sdp.hpp
            ./src/format/sdp/sdp.cpp
            ./src/format/opus_header.hpp
            ./src/format/opus_header.cpp
            ./src/net/stun/stun.hpp
            ./src/net/stun/stun.cpp
            ./src/utils/byte_crypto.hpp
            ./src/utils/byte_crypto.cpp)
target_link_libraries(rtc openssl libsrtp uv ${LINK_LIBS})

################################################################
## whip streamer module
add_library(whip SHARED ./src/net/webrtc/whip.cpp)
target_link_libraries(whip rtc)

################################################################
## mspush streamer module
add_library(mspush SHARED 
            ./src/net/webrtc/ms_push.cpp 
            ./src/net/webrtc/mediasoup_pub.cpp)
target_link_libraries(mspush rtc)

################################################################
## whep streamer module
add_library(whep SHARED ./src/net/webrtc/whep.cpp)
target_link_libraries(whep rtc)

################################################################
## mspull streamer module
add_library(mspull SHARED 
            ./src/net/webrtc/ms_pull.cpp 
            ./src/net/webrtc/mediasoup_pub.cpp)
target_link_libraries(mspull rtc)


add_subdirectory(src/tools)
