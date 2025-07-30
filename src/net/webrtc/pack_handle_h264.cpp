#include "pack_handle_h264.hpp"
#include "h264_h265_header.hpp"
#include "utils/av/media_packet.hpp"
#include "logger.hpp"
#include "byte_stream.hpp"
#include "timeex.hpp"
#include <cstdint>

namespace cpp_streamer
{
static const uint8_t NAL_START_CODE[4] = {0, 0, 0, 1};
static const size_t STAPA_FIELD_SIZE = 2;

PackHandleH264::PackHandleH264(PackCallbackI* cb, uv_loop_t* loop, Logger* logger)
    :TimerInterface(loop, 100), cb_(cb), logger_(logger) {
    StartTimer();
}

PackHandleH264::~PackHandleH264() {
    StopTimer();
}

void PackHandleH264::GetStartEndBit(RtpPacket* pkt, bool& start, bool& end) {
    uint8_t* payload_data = pkt->GetPayload();
    uint8_t fu_header = payload_data[1 + is_h265_];

    start = false;
    end   = false;

    if ((fu_header & 0x80) != 0) {
        start = true;
    }

    if ((fu_header & 0x40) != 0) {
        end = true;
    }
}

void PackHandleH264::OnTimer() {
    CheckFuaTimeout();
}

void PackHandleH264::InputRtpPacket(std::shared_ptr<RtpPacketInfo> pkt_ptr) {
    if (!init_flag_) {
        init_flag_ = true;
        last_extend_seq_ = pkt_ptr->extend_seq_;
    } else {
        if ((last_extend_seq_ + 1) != pkt_ptr->extend_seq_) {
            packets_queue_.clear();
            start_flag_ = false;
            end_flag_   = false;

            ReportLost(pkt_ptr);
            last_extend_seq_ = pkt_ptr->extend_seq_;
            return;
        }
        last_extend_seq_ = pkt_ptr->extend_seq_;
    }

    uint8_t* payload_data = pkt_ptr->pkt->GetPayload();
    uint8_t nal_type = is_h265_ ? payload_data[0] >> 1 & 0x3f : payload_data[0] & 0x1f;

    if (nal_type <= max_nal_type_) {//single nalu
        int64_t dts = pkt_ptr->pkt->GetTimestamp();
        size_t pkt_size = sizeof(NAL_START_CODE) + pkt_ptr->pkt->GetPayloadLength() + 1024;

        auto pkt = std::make_shared<Media_Packet>(pkt_size);
        pkt->buffer_ptr_->AppendData((char*)NAL_START_CODE, sizeof(NAL_START_CODE));
        pkt->buffer_ptr_->AppendData((char*)payload_data, pkt_ptr->pkt->GetPayloadLength());
        pkt->dts_        = dts;
        pkt->pts_        = dts;
        OutputPacket(pkt);
        return;
    } else if (nal_type == fu_nal_type_) {//rtp fua
        bool start = false;
        bool end   = false;
        GetStartEndBit(pkt_ptr->pkt, start, end);
        if (start && !end) {
            packets_queue_.clear();
            start_flag_ = start;
        } else if (start && end) {//exception happened
            LogErrorf(logger_, "rtp h264 pack error: both start and end flag are enable");
            ResetRtpFua();
            ReportLost(pkt_ptr);
            return;
        }

        if (end && !start_flag_) {
            LogErrorf(logger_, "rtp h264 pack error: get end rtp packet but there is no start rtp packet");
            ResetRtpFua();
            ReportLost(pkt_ptr);
            return;
        }

        if (end) {
            end_flag_ = true;
        }

        packets_queue_.push_back(pkt_ptr);

        if (start_flag_ && end_flag_) {
            auto pkt = std::make_shared<Media_Packet>(50*1024);
            int64_t dts = 0;
            bool ok = DemuxFua(pkt, dts);
            if (ok) {
                pkt->dts_        = dts;
                pkt->pts_        = dts;
                OutputPacket(pkt);
            } else {
                ReportLost(pkt_ptr);
            }
            start_flag_ = false;
            end_flag_   = false;
            return;
        }

        CheckFuaTimeout();
        return;
    } else if (nal_type == stap_nal_type_) {//handle stapA
        bool ret = DemuxStapA(pkt_ptr);
        if (!ret) {
            ReportLost(pkt_ptr);
        }
    }

    return;
}

void PackHandleH264::ReportLost(std::shared_ptr<RtpPacketInfo> pkt_ptr) {
    int64_t now_ms = now_millisec();

    if ((now_ms - report_lost_ts_) > 500) {
        report_lost_ts_ = now_ms;
        cb_->PackHandleReset(pkt_ptr);
    }
}

void PackHandleH264::CheckFuaTimeout() {
    size_t queue_len = packets_queue_.size();
    int64_t now_ms = now_millisec();
    for (size_t index = 0; index < queue_len; index++) {
        std::shared_ptr<RtpPacketInfo> pkt_ptr = packets_queue_.front();
        int64_t pkt_local_ms = pkt_ptr->pkt->GetLocalMs();
        if ((now_ms - pkt_local_ms) < PACK_BUFFER_TIMEOUT) {
            break;
        }
        packets_queue_.pop_front();
        LogWarnf(logger_, "h264 fua list is timeout, packet pop seq:%d", pkt_ptr->pkt->GetSeq());
    }
    return;
}

bool PackHandleH264::DemuxStapA(std::shared_ptr<RtpPacketInfo> pkt_ptr) {
    uint8_t* payload_data = pkt_ptr->pkt->GetPayload();
    size_t payload_length = pkt_ptr->pkt->GetPayloadLength();
    if (payload_length <= 1 + is_h265_ + STAPA_FIELD_SIZE) {
        LogErrorf(logger_, "demux stapA error: payload length(%lu) is too short", payload_length);
        return false;
    }

    std::vector<size_t> offsets;
    bool ret = ParseStapAOffsets(payload_data, payload_length, offsets);
    if (!ret) {
        return ret;
    }
    int64_t dts = (int64_t)pkt_ptr->pkt->GetTimestamp();

    offsets.push_back(payload_length + STAPA_FIELD_SIZE);//end offset.
    for (size_t index = 0; index < (offsets.size() - 1); index++) {
        size_t start_offset = offsets[index];
        size_t end_offset = offsets[index + 1] - STAPA_FIELD_SIZE;
        if ((end_offset - start_offset) < 1 + is_h265_) {
            LogErrorf(logger_, "demux stapA error: start offset:%lu, end offset:%lu",
                start_offset,  end_offset);
            return false;
        }
        size_t pkt_size = sizeof(NAL_START_CODE) + end_offset - start_offset + 1024;
        auto pkt = std::make_shared<Media_Packet>(pkt_size);
        pkt->buffer_ptr_->AppendData((char*)NAL_START_CODE, sizeof(NAL_START_CODE));
        pkt->buffer_ptr_->AppendData((char*)payload_data + start_offset, end_offset - start_offset);
        pkt->dts_ = dts;
        pkt->pts_ = dts;
        OutputPacket(pkt);
    }
    return true;
}

bool PackHandleH264::ParseStapAOffsets(const uint8_t* data, size_t data_len, std::vector<size_t> &offsets) {
    
    size_t offset = is_h265_ + 1;
    size_t left_len = data_len;
    const uint8_t* p = data + offset;
    left_len -= offset;

    while (left_len > 0) {
        if (left_len < sizeof(uint16_t)) {
            LogErrorf(logger_, "h264 stapA nalu len error: left length(%lu) is not enough.", left_len);
            return false;
        }
        uint16_t nalu_len = ByteStream::Read2Bytes(p);
        p += sizeof(uint16_t);
        left_len -= sizeof(uint16_t);
        if (nalu_len > left_len) {
            LogErrorf(logger_, "h264 stapA nalu len error: left length(%lu) is smaller than nalu length(%d).",
                    left_len, nalu_len);
            return false;
        }
        p += nalu_len;
        left_len -= nalu_len;
        offsets.push_back(offset + STAPA_FIELD_SIZE);
        offset += STAPA_FIELD_SIZE + nalu_len;
    }
    
    return true;
}

bool PackHandleH264::DemuxFua(Media_Packet_Ptr h264_pkt_ptr, int64_t& timestamp) {
    size_t queue_len = packets_queue_.size();
    bool has_start = false;
    bool has_end   = false;

    std::shared_ptr<DataBuffer> buffer_ptr = h264_pkt_ptr->buffer_ptr_;
    for (size_t index = 0; index < queue_len; index++) {
        bool start = false;
        bool end   = false;

        std::shared_ptr<RtpPacketInfo> pkt_ptr = packets_queue_.front();
        packets_queue_.pop_front();

        timestamp = (int64_t)pkt_ptr->pkt->GetTimestamp();
        GetStartEndBit(pkt_ptr->pkt, start, end);

        uint8_t* payload   = pkt_ptr->pkt->GetPayload();
        size_t payload_len = pkt_ptr->pkt->GetPayloadLength();
        if (index == 0) {
            if (start) {
                has_start = true;
                buffer_ptr->AppendData((char*)NAL_START_CODE, sizeof(NAL_START_CODE));
                if (is_h265_) {
                    uint8_t nalu_header[2];
                    uint8_t type = payload[2] & 0x3f;
                    nalu_header[0] = (type << 1) | (payload[0] & 0x81);
                    nalu_header[1] = payload[1];
                    buffer_ptr->AppendData((char*)&nalu_header, sizeof(nalu_header));
                } else {
                    uint8_t fu_indicator = payload[0];
                    uint8_t fu_header    = payload[1];
                    uint8_t nalu_header  = (fu_indicator & 0xe0) | (fu_header & 0x1f);
                    buffer_ptr->AppendData((char*)&nalu_header, sizeof(nalu_header));
                }
                buffer_ptr->AppendData((char*)payload + 2 + is_h265_, payload_len - 2 - is_h265_);
            }
        } else {
            if (has_start) {
                buffer_ptr->AppendData((char*)payload + 2 + is_h265_, payload_len - 2 - is_h265_);
            }
        }
        if (end) {
            has_end = true;
            break;
        }
    }
    if (!has_start || !has_end) {
        LogErrorf(logger_, "rtp h264 demux fua error: has start:%d, has end:%d", has_start, has_end);
        return false;
    }
    return true;
}

void PackHandleH264::ResetRtpFua() {
    start_flag_ = false;
    end_flag_   = false;
    packets_queue_.clear();
}

void PackHandleH264::OutputPacket(Media_Packet_Ptr pkt) {
    if (!cb_ || !pkt) return;
    pkt->av_type_ = MEDIA_VIDEO_TYPE;
    pkt->fmt_type_ = MEDIA_FORMAT_RAW;
    pkt->is_seq_hdr_ = false;
    pkt->is_key_frame_ = false;
    uint8_t nal_type = ((uint8_t*)pkt->buffer_ptr_->Data())[4];
    if (is_h265_) {
        pkt->codec_type_ = MEDIA_CODEC_H265;
        nal_type = nal_type >> 1 & 0x3f; // H265 NALU type is in the first 6 bits
        if(nal_type == NAL_UNIT_SPS || nal_type == NAL_UNIT_PPS || nal_type == NAL_UNIT_VPS) {
            pkt->is_seq_hdr_ = true;
            pkt->is_key_frame_ = false;
        } else if (nal_type == NAL_UNIT_CODED_SLICE_IDR || nal_type == NAL_UNIT_CODED_SLICE_IDR_N_LP) {
            pkt->is_seq_hdr_ = false;
            pkt->is_key_frame_ = true;
        }
    } else {
        pkt->codec_type_ = MEDIA_CODEC_H264;
        nal_type = nal_type & 0x1f; // H264 NALU type is in the last 5 bits
        if ((nal_type == kAvcNaluTypeSPS) || (nal_type == kAvcNaluTypePPS)) {
            pkt->is_seq_hdr_ = true;
            pkt->is_key_frame_ = false;
        }
        else if (nal_type == kAvcNaluTypeIDR) {
            pkt->is_seq_hdr_ = false;
            pkt->is_key_frame_ = true;
        }
    }
    cb_->MediaPacketOutput(pkt);
}

}
