#ifndef RTC_STREAM_PUB_HPP
#define RTC_STREAM_PUB_HPP

#include <stdint.h>

namespace cpp_streamer
{
#define RTT_DEFAULT 30 //ms
#define RETRANSMIT_MAX_COUNT 20

class RtcSendStreamCallbackI
{
public:
    virtual void SendRtpPacket(uint8_t* data, size_t len) = 0;
    virtual void SendRtcpPacket(uint8_t* data, size_t len) = 0;
};

}

#endif
