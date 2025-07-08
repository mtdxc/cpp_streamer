#include "rtp_h264_pack.hpp"
#include "net/rtprtcp/rtprtcp_pub.hpp"
#include "net/rtprtcp/rtp_packet.hpp"

#include <cstring>
#include <stdio.h>

namespace cpp_streamer
{

RtpPacket* GenerateStapAPackets(std::vector<std::pair<unsigned char*, int>> NaluVec, HeaderExtension* ext) {
    size_t data_len = 0;

    data_len += kNalHeaderSize;
    for (auto& nalu : NaluVec) {
        data_len += kLengthFieldSize;
        data_len += nalu.second;
    }
    RtpPacket* packet = MakeRtpPacket(ext, data_len);

    uint8_t* payload = packet->GetPayload();
    auto nalu        = NaluVec[0];
    auto nalu_header = (nalu.first)[0];
    payload[0]       = (nalu_header & (kFBit | kNriMask)) | NaluType::kStapA;
    size_t index     = kNalHeaderSize;

    for (auto& nalu : NaluVec)
    {
        payload[index]     = nalu.second >> 8;
        payload[index + 1] = nalu.second;
        index += kLengthFieldSize;
        memcpy(&payload[index], nalu.first, nalu.second);
        index += nalu.second;
    }
    packet->SetPayloadLength(index);

    return packet;
}

#define kH265NalHeaderSize 2
// HEVC FU-A fragmentation according to RFC 7798
std::vector<RtpPacket*> GenerateFuAPackets265(uint8_t* nalu, size_t nalu_size, HeaderExtension* ext) {
    std::vector<RtpPacket*> packets;
    if (nalu_size < kH265NalHeaderSize) return packets;

    size_t payload_left = nalu_size - kH265NalHeaderSize;
    std::vector<int> fragment_sizes = SplitNalu(payload_left);

    uint8_t nalu_header0 = nalu[0];
    uint8_t nalu_header1 = nalu[1];
    uint8_t nalu_type = (nalu_header0 >> 1) & 0x3F; // 6 bits

    auto fragment = nalu + kH265NalHeaderSize;

    for (size_t i = 0; i < fragment_sizes.size(); ++i) {
        size_t payload_len = 3 + fragment_sizes[i]; // 2 bytes FU indicator/header + 1 byte FU header + fragment
        RtpPacket* packet = MakeRtpPacket(ext, payload_len);
        uint8_t* payload = packet->GetPayload();

        // FU indicator: F(1) | Type(6) | LayerId(6) | TID(3)
        payload[0] = 49 << 1; // F bit and type 49 (FU)
        payload[1] = nalu_header1; // LayerId and TID from original NALU

        // FU header: S(1) | E(1) | R(1) | Type(6)
        payload[2] = 0;
        if (i == 0) payload[2] |= 0x80; // Start bit
        if (i == fragment_sizes.size() - 1) payload[2] |= 0x40; // End bit
        payload[2] |= nalu_type;

        memcpy(payload + 3, fragment, fragment_sizes[i]);
        fragment += fragment_sizes[i];

        packet->SetPayloadLength(3 + fragment_sizes[i]);
        packet->SetMarker(i == (fragment_sizes.size() - 1));
        packets.push_back(packet);
    }
    return packets;
}

std::vector<RtpPacket*> GenerateFuAPackets(uint8_t* nalu, size_t nalu_size, HeaderExtension* ext) {
    std::vector<RtpPacket*> packets;
    size_t payload_left             = nalu_size - kNalHeaderSize;
    std::vector<int> fragment_sizes = SplitNalu(payload_left);
    uint8_t nalu_header             = nalu[0];
    auto fragment                   = nalu + kNalHeaderSize;

    for (size_t i = 0; i < fragment_sizes.size(); ++i) {
        size_t payload_len = kFuAHeaderSize + fragment_sizes[i];
        RtpPacket* packet = MakeRtpPacket(ext, payload_len);
        uint8_t* payload   = packet->GetPayload();

        uint8_t fu_indicator = (nalu_header & (kFBit | kNriMask)) | NaluType::kFuA;

        uint8_t fu_header = 0;
        // S | E | R | 5 bit type.
        fu_header |= (i == 0 ? kSBit : 0);
        fu_header |= (i == (fragment_sizes.size() - 1) ? kEBit : 0);

        uint8_t type = nalu_header & kTypeMask;
        fu_header |= type;

        payload[0] = fu_indicator;
        payload[1] = fu_header;

        memcpy(payload + kFuAHeaderSize, fragment, fragment_sizes[i]);

        fragment += fragment_sizes[i];

        packet->SetPayloadLength(kFuAHeaderSize + fragment_sizes[i]);
        packet->SetMarker(i == (fragment_sizes.size() - 1));
        packets.push_back(packet);
    }
    return packets;
}

std::vector<int> SplitNalu(int payload_len) {
    std::vector<int> payload_sizes;
    size_t payload_left = payload_len;

    while (payload_left > 0) {
        if (payload_left > kPayloadMaxSize) {
            payload_sizes.push_back(kPayloadMaxSize);
            payload_left -= kPayloadMaxSize;
        }
        else {
            payload_sizes.push_back(payload_left);
            payload_left = 0;
        }
    }
    return payload_sizes;
}

}
