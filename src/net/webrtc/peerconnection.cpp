#include "peerconnection.hpp"
#include "stun.hpp"
#include "rtprtcp_pub.hpp"
#include "rtcp_rr.hpp"
#include "rtcp_sr.hpp"
#include "rtcp_xr_dlrr.hpp"
#include "rtcp_xr_rrt.hpp"
#include "rtcpfb_nack.hpp"
#include "srtp_session.hpp"
#include "uuid.hpp"
#include "byte_crypto.hpp"
#include "pack_handle_h264.hpp"
#include "pack_handle_audio.hpp"
#include "h264_h265_header.hpp"

#include <cstring>
#include <sstream>
#include <set>

namespace cpp_streamer
{
#define RTCP_RR_INTERVAL (1*1000)

RTP_EXT_TYPE GetRtpExtType(const std::string& uri) {
    RTP_EXT_TYPE ret_type;
    if (uri == "urn:ietf:params:rtp-hdrext:sdes:mid") {
        ret_type = MID_TYPE;
    }
    else if (uri == "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id") {
        ret_type = RTP_STREAMID_TYPE;
    }
    else if (uri == "urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id") {
        ret_type = RP_RTP_STREAMID_TYPE;
    }
    else if (uri == "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time") {
        ret_type = ABS_SEND_TIME_TYPE;
    }
    else if (uri == "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01") {
        ret_type = TCC_WIDE_TYPE;
    }
    else if (uri == "urn:ietf:params:rtp-hdrext:ssrc-audio-level") {
        ret_type = SSRC_AUDIO_LEVEL_TYPE;
    }
    else if (uri == "http://tools.ietf.org/html/draft-ietf-avtext-framemarking-07") {
        ret_type = AVTEXT_FRAMEMARKING_TYPE;
    }
    else if (uri == "urn:ietf:params:rtp-hdrext:framemarking") {
        ret_type = RTP_HDREXT_FRAMEMARKING_TYPE;
    }
    else if (uri == "urn:3gpp:video-orientation") {
        ret_type = VIDEO_ORIENTATION_TYPE;
    }
    else if (uri == "urn:ietf:params:rtp-hdrext:toffset") {
        ret_type = TOFFSET_TYPE;
    }
    else if (uri == "http://www.webrtc.org/experiments/rtp-hdrext/abs-capture-time") {
        ret_type = ABS_CAPTURE_TIME_TYPE;
    }
    else {
        CSM_THROW_ERROR("unknown rtp ext type:%s", uri.c_str());
    }
    return ret_type;
}

const char* GetDirectionString(WebRtcSdpDirection dir) {
    switch (dir)
    {
    case SEND_ONLY:
        return "sendonly";
    case RECV_ONLY:
        return "recvonly";
    case SEND_RECV:
        return "sendrecv";
    default:
        return "";
    }
    return "";
}

PeerConnection::PeerConnection(uv_loop_t* loop, Logger* logger, PCStateReportI* state_report)
    :TimerInterface(loop, 300), loop_(loop), logger_(logger), state_report_(state_report)
    , dtls_(this, logger)
    , offer_sdp_(&dtls_, logger)
    , answer_sdp_(&dtls_, logger)
    , jb_video_(MEDIA_VIDEO_TYPE, this, loop, logger)
    , jb_audio_(MEDIA_AUDIO_TYPE, this, loop, logger)
{
    udp_client_ = new UdpClient(loop, this, logger, nullptr, 0);
    dtls_.udp_client_ = udp_client_;

    SRtpSession::Init(logger);
}

PeerConnection::~PeerConnection()
{
    LogInfof(logger_, "destruct PeerConnection");
    StopTimer();
    if (udp_client_) {
        delete udp_client_;
        udp_client_ = nullptr;
    }
}

int PeerConnection::ParseAnswerSdp(const std::string& sdp) {
    int ret = answer_sdp_.Parse(sdp);
    if (ret < 0) {
        LogErrorf(logger_, "answer sdp parse error");
        return ret;
    }

    if (answer_sdp_.ssrc_info_map_.empty()) {
        answer_sdp_.ssrc_info_map_ = offer_sdp_.ssrc_info_map_;
    }
    LogInfof(logger_, "audio ssrc:%u@%d%s, video ssrc:%u@%d%s, video rtx %d ssrc:%u@%d",
        answer_sdp_.GetAudioSsrc(), answer_sdp_.GetAudioPayloadType(), answer_sdp_.IsAudioNackEnable() ? " nack" : "",
        answer_sdp_.GetVideoSsrc(), answer_sdp_.GetVideoPayloadType(), answer_sdp_.IsVideoNackEnable()?" nack":"",
        answer_sdp_.IsVideoRtxEnable(), answer_sdp_.GetVideoRtxSsrc(), answer_sdp_.GetVideoRtxPayloadType());
    /*
    LogInfof(logger_, "video rate:%d, video rtx rate:%d, audio rate:%d",
            answer_sdp_.GetVideoClockRate(),
            answer_sdp_.GetVideoRtxClockRate(),
            answer_sdp_.GetAudioClockRate());
    */
    if (pc_state_ >= PC_SDP_DONE_STATE) {
        LogWarnf(logger_, "ParseAnswerSdp error:  peer connection state is %d", pc_state_);
        return 0;
    }
    pc_state_ = PC_SDP_DONE_STATE;
    //LogInfof(logger_, "answer sdp:%s", answer_sdp_.GenSdpString().c_str());
    Report("sdp", "ready");

    StartTimer();
    return ret;
}


void PeerConnection::SendStun(int64_t now_ms) {
    if (pc_state_ < PC_SDP_DONE_STATE) {
        return;
    }

    if (last_stun_ms_ <= 0) {
        last_stun_ms_ = now_ms;
    } else {
        if (now_ms - last_stun_ms_ < 800) {
            return;
        }
        last_stun_ms_ = now_ms;
    }

    if (dtls_.ice_infos.empty()) {
        LogErrorf(logger_, "dtls ice information is empty");
        return;
    }
    // @todo 只往第一个地址发
    IceInfo& ice = dtls_.ice_infos[0];
    assert(ice.net_type == ICE_UDP);

    UdpTuple remote_address(ice.hostip, ice.port);
    dtls_.remote_address_ = remote_address;

    StunPacket pkt;
    pkt.username_ = dtls_.remote_fragment_;
    pkt.username_ += ":";
    pkt.username_ += dtls_.local_fragment_;
    LogDebugf(logger_, "stun username:%s", pkt.username_.c_str());
    LogDebugf(logger_, "dtls remote frag:%s", dtls_.remote_fragment_.c_str());
    LogDebugf(logger_, "dtls local frag:%s", dtls_.local_fragment_.c_str());
    pkt.password_ = dtls_.remote_pwd_;
    pkt.has_use_candidate_ = true;
    pkt.add_msg_integrity_ = true;
    pkt.priority_ = 100;

    pkt.Serialize();

    udp_client_->Write((char*)pkt.data_, pkt.data_len_, remote_address);
    udp_client_->TryRead();
}

std::string PeerConnection::CreateOfferSdp(WebRtcSdpDirection direction_type) {
    direct_type_ = direction_type;
    offer_sdp_.direction_ = GetDirectionString(direction_type);
    offer_sdp_.SetVideoRtxFlag(true);

    LogInfof(logger_, "peer connection create offer sdp, direct:%s", GetDirectionString(direction_type));

    //start:mid, msid
    offer_sdp_.vmid_ = 0;
    offer_sdp_.amid_ = 1;

    offer_sdp_.v_msid_ = UUID::MakeUUID();
    offer_sdp_.a_msid_ = UUID::MakeUUID();

    offer_sdp_.v_msid_appdata_ = UUID::MakeUUID2();
    offer_sdp_.a_msid_appdata_ = UUID::MakeUUID2();
    //end:mid, msid
    
    //video audio payload list
    offer_sdp_.video_pt_vec_.push_back(VPLAYLOAD_DEF_TYPE);
    offer_sdp_.video_pt_vec_.push_back(RTX_PAYLOAD_DEF_TYPE);

    offer_sdp_.audio_pt_vec_.push_back(APLAYLOAD_DEF_TYPE);

    //start: audio/video RtpMap
    RtpMapInfo h264RtpInfo = {};
    h264RtpInfo.payload_type = VPLAYLOAD_DEF_TYPE;
    h264RtpInfo.codec_type   = "H264";
    h264RtpInfo.clock_rate   = 90000;
    offer_sdp_.video_rtp_map_infos_[VPLAYLOAD_DEF_TYPE] = h264RtpInfo;

    RtpMapInfo rtxRtpInfo = {};
    rtxRtpInfo.payload_type = RTX_PAYLOAD_DEF_TYPE;
    rtxRtpInfo.codec_type   = "rtx";
    rtxRtpInfo.clock_rate   = 90000;
    offer_sdp_.video_rtp_map_infos_[RTX_PAYLOAD_DEF_TYPE] = rtxRtpInfo;

    RtpMapInfo opusRtpInfo = {};
    opusRtpInfo.payload_type = APLAYLOAD_DEF_TYPE;
    opusRtpInfo.codec_type   = "opus";
    opusRtpInfo.clock_rate   = 48000;
    opusRtpInfo.channel      = 2;
    offer_sdp_.audio_rtp_map_infos_[APLAYLOAD_DEF_TYPE] = opusRtpInfo;
    //end: audio/video RtpMap
    
    //start: audio/video fmtp info
    FmtpInfo h264FmtpInfo = {};
    h264FmtpInfo.payload_type = VPLAYLOAD_DEF_TYPE;
    h264FmtpInfo.attr_string  = "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f";
    h264FmtpInfo.is_video     = true;
    h264FmtpInfo.is_rtx       = false;
    h264FmtpInfo.rtx_payload_type = RTX_PAYLOAD_DEF_TYPE;
    offer_sdp_.video_fmtp_vec_.push_back(h264FmtpInfo);

    std::string rtxFmtpStr = "apt=";
    rtxFmtpStr += std::to_string(VPLAYLOAD_DEF_TYPE);
    FmtpInfo rtxFmtpInfo = {};
    rtxFmtpInfo.payload_type = RTX_PAYLOAD_DEF_TYPE;
    rtxFmtpInfo.attr_string  = rtxFmtpStr;
    rtxFmtpInfo.is_video     = true;
    rtxFmtpInfo.is_rtx       = true;
    rtxFmtpInfo.rtx_payload_type = 0;
    offer_sdp_.video_fmtp_vec_.push_back(rtxFmtpInfo);

    FmtpInfo opusFmtpInfo = {};
    opusFmtpInfo.payload_type = APLAYLOAD_DEF_TYPE;
    opusFmtpInfo.attr_string  = "minptime=10;useinbandfec=1";
    opusFmtpInfo.is_video     = false;
    opusFmtpInfo.is_rtx       = false;
    opusFmtpInfo.rtx_payload_type = 0;
    offer_sdp_.audio_fmtp_vec_.push_back(opusFmtpInfo);
    //end: audio/video fmtp info
    
    //start: rtcpFb
    offer_sdp_.video_rtcpfb_vec_.push_back({VPLAYLOAD_DEF_TYPE, "goog-remb"});
    offer_sdp_.video_rtcpfb_vec_.push_back({VPLAYLOAD_DEF_TYPE,"transport-cc"});
    offer_sdp_.audio_rtcpfb_vec_.push_back({APLAYLOAD_DEF_TYPE, "transport-cc"});
    offer_sdp_.video_rtcpfb_vec_.push_back({VPLAYLOAD_DEF_TYPE, "cmm fir"});
    offer_sdp_.video_rtcpfb_vec_.push_back({VPLAYLOAD_DEF_TYPE, "nack"});
    offer_sdp_.video_rtcpfb_vec_.push_back({VPLAYLOAD_DEF_TYPE, "nack pli"});
    //end: rtcpFb
    
    //Start: ssrc info
    offer_sdp_.video_ssrc_ = ByteCrypto::GetRandomUint(1, 0xffffffff);
    offer_sdp_.video_rtx_ssrc_ = ByteCrypto::GetRandomUint(1, 0xffffffff);
    offer_sdp_.audio_ssrc_ = ByteCrypto::GetRandomUint(1, 0xffffffff);

    offer_sdp_.video_cname_ = ByteCrypto::GetRandomString(16);
    offer_sdp_.audio_cname_ = ByteCrypto::GetRandomString(16);

    LogInfof(logger_, "audio srrc:%u, video ssrc:%u, rtx ssrc:%u", 
        offer_sdp_.audio_ssrc_, offer_sdp_.video_ssrc_, offer_sdp_.video_rtx_ssrc_);
    SSRCInfo video_ssrc_info = {};
    video_ssrc_info.ssrc = offer_sdp_.video_ssrc_;
    video_ssrc_info.is_video = true;
    video_ssrc_info.is_rtx   = false;
    video_ssrc_info.msid     = offer_sdp_.v_msid_;
    video_ssrc_info.msid_appdata = offer_sdp_.v_msid_appdata_;
    video_ssrc_info.cname = offer_sdp_.video_cname_;
    video_ssrc_info.rtx_ssrc = offer_sdp_.video_rtx_ssrc_;
    offer_sdp_.ssrc_info_map_[offer_sdp_.video_ssrc_] = video_ssrc_info;

    SSRCInfo rtx_ssrc_info = {};
    rtx_ssrc_info.ssrc = offer_sdp_.video_rtx_ssrc_;
    rtx_ssrc_info.is_video = true;
    rtx_ssrc_info.is_rtx = true;
    rtx_ssrc_info.msid = offer_sdp_.v_msid_;
    rtx_ssrc_info.msid_appdata = offer_sdp_.v_msid_appdata_;
    rtx_ssrc_info.cname = offer_sdp_.video_cname_;
    rtx_ssrc_info.rtx_ssrc = offer_sdp_.video_ssrc_;

    offer_sdp_.ssrc_info_map_[offer_sdp_.video_rtx_ssrc_] = rtx_ssrc_info;

    SSRCInfo audio_ssrc_info = {};
    audio_ssrc_info.ssrc = offer_sdp_.audio_ssrc_;
    audio_ssrc_info.is_video = false;
    audio_ssrc_info.is_rtx   = false;
    audio_ssrc_info.msid     = offer_sdp_.a_msid_;
    audio_ssrc_info.msid_appdata = offer_sdp_.a_msid_appdata_;
    audio_ssrc_info.cname = offer_sdp_.audio_cname_;
    audio_ssrc_info.rtx_ssrc = 0;
    offer_sdp_.ssrc_info_map_[offer_sdp_.audio_ssrc_] = audio_ssrc_info;
    //end: ssrc info
    
    //start: ExtMap
    offer_sdp_.em_toffset_         = 1;
    offer_sdp_.em_abs_send_time_   = 2;
    offer_sdp_.em_video_rotation_  = 3;
    offer_sdp_.em_tcc_             = 4;
    offer_sdp_.em_playout_delay_   = 5;
    offer_sdp_.em_video_content_   = 6;
    offer_sdp_.em_video_timing_    = 7;
    offer_sdp_.em_color_space_     = 8;
    offer_sdp_.em_sdes_            = 9;
    offer_sdp_.em_rtp_streamid_    = 10;
    offer_sdp_.em_rp_rtp_streamid_ = 11;

    offer_sdp_.em_audio_level_     = 14;
    //end: ExtMap
    
    if (dtls_.Init() < 0) {
        LogErrorf(logger_, "dtls Init error");
        return "";
    }
    return offer_sdp_.GenSdpString(true, true);
}

int PeerConnection::HandleRtcpSr(uint8_t* data, int len) {
    int ret = sizeof(RtcpCommonHeader) + sizeof(uint32_t) + sizeof(RtcpSrBlock);
    try {
        RtcpSrPacket* rr_pkt = RtcpSrPacket::Parse(data, len);
        uint32_t ssrc = rr_pkt->GetSsrc();
        if (video_recv_stream_ && ssrc == video_recv_stream_->GetSsrc()) {
            video_recv_stream_->HandleRtcpSr(rr_pkt);
        } else if (audio_recv_stream_ && ssrc == audio_recv_stream_->GetSsrc()) {
            audio_recv_stream_->HandleRtcpSr(rr_pkt);
        } else {
            LogInfof(logger_, "unkown rtcp sr ssrc:%u", ssrc);
        }
        delete rr_pkt;
    } catch(CppStreamException& e) {
        LogErrorf(logger_, "handle rtcp rr exception:%s", e.what());
        ret = -1;
    }
    return ret;
}

int PeerConnection::HandleRtcpRr(uint8_t* data, int len) {
    int ret = sizeof(RtcpCommonHeader) + sizeof(RtcpRrBlock);
    if (len <= ret) {
        return len;
    }

    RtcpRrPacket* rr_pkt = RtcpRrPacket::Parse(data, len);

    //LogDebugf(logger_, "rtcp rr dump:%s", rr_pkt->Dump().c_str());
    try {
        for (RtcpRrBlockInfo& block_info : rr_pkt->GetRrBlocks()) {
            uint32_t ssrc = block_info.GetReporteeSsrc();

            if (video_send_stream_ && ssrc == video_send_stream_->GetSsrc()) {
                video_send_stream_->HandleRtcpRr(block_info);
            }

            if (audio_send_stream_ && ssrc == audio_send_stream_->GetSsrc()) {
                audio_send_stream_->HandleRtcpRr(block_info);
            }
 
        }

    } catch (CppStreamException& e) {
        LogErrorf(logger_, "handle rtcp rr exception:%s", e.what());
    }

    delete rr_pkt;
    return len;
}

int PeerConnection::HandleRtcpRtpFb(uint8_t* data, int data_len) {
    if (data_len <= (int)sizeof(RtcpFbCommonHeader)) {
        return data_len;
    }

    RtcpFbCommonHeader* header = (RtcpFbCommonHeader*)data;
    switch (header->fmt)
    {
        case FB_RTP_NACK:
        {
            RtcpFbNack* nack_pkt = nullptr;
            try {
                nack_pkt = RtcpFbNack::Parse(data, data_len);
                uint32_t ssrc = nack_pkt->GetMediaSsrc();

                if (video_send_stream_ && ssrc == video_send_stream_->GetSsrc()) {
                    video_send_stream_->HandleRtcpNack(nack_pkt);
                }

                if (audio_send_stream_ && ssrc == audio_send_stream_->GetSsrc()) {
                    audio_send_stream_->HandleRtcpNack(nack_pkt);
                }
                delete nack_pkt;
            } catch(CppStreamException& e) {
                LogErrorf(logger_, "rtcp feedback nack error:%s", e.what());
                return -1;
            }
            break;
        }
        default:
        {
            LogWarnf(logger_, "receive rtcp psfb format(%d) is not handled.", header->fmt);
            break;
        }
    }
    return data_len;
}

int PeerConnection::HandleRtcpPsFb(uint8_t* data, int data_len) {

    return data_len;
}


int PeerConnection::HandleXrDlrr(XrDlrrData* dlrr_block) {
    uint32_t ssrc = ntohl(dlrr_block->ssrc);

    LogDebugf(logger_, "Handle Xr Dlrr ssrc:%u", ssrc);
    if (video_recv_stream_ && ssrc == video_recv_stream_->GetSsrc()) {
        video_recv_stream_->HandleXrDlrr(dlrr_block);
    }

    if (audio_recv_stream_ && ssrc == audio_recv_stream_->GetSsrc()) {
        audio_recv_stream_->HandleXrDlrr(dlrr_block);
    }
    return 0;
}

int PeerConnection::HandleRtcpXr(uint8_t* data, int data_len) {
    RtcpCommonHeader* header = (RtcpCommonHeader*)data;
    uint32_t* ssrc_p         = (uint32_t*)(header + 1);
    XrCommonData* xr_hdr     = (XrCommonData*)(ssrc_p + 1);
    int64_t xr_len           = data_len - sizeof(RtcpCommonHeader) - 4;

    while(xr_len > 0) {
        switch(xr_hdr->bt)
        {
            //handle dlrr as receiver
            case XR_DLRR:
            {
                XrDlrrData* dlrr_block = (XrDlrrData*)(xr_hdr + 1);
                HandleXrDlrr(dlrr_block);
                break;
            }
            //handle rrt as sender
            case XR_RRT:
            {
                XrRrtData* rrt_block = (XrRrtData*)(xr_hdr + 1);
                last_xr_ntp_.ntp_sec  = ntohl(rrt_block->ntp_sec);
                last_xr_ntp_.ntp_frac = ntohl(rrt_block->ntp_frac);
                last_xr_ms_ = now_millisec();
                break;
            }
            default:
            {
                LogErrorf(logger_, "handle unkown xr type:%d", xr_hdr->bt);
            }
        }
        int64_t offset = 4 + ntohs(xr_hdr->block_length)*4;
        xr_len -= offset;
        data   += offset;
        xr_hdr = (XrCommonData*)data;
    }
    return data_len;
}

void PeerConnection::HandleRtcp(uint8_t* data, size_t len) {
    if (!read_srtp_) {
        LogWarnf(logger_, "read srtp is not ready");
        return;
    }
    if (read_srtp_->DecryptSrtcp(data, &len) == false) {
        LogErrorf(logger_, "read srtp decrypt srtcp error, len:%lu", len);
        return;
    }
    //handle rtcp packet
    int left_len = (int)len;
    uint8_t* p = data;

    while (left_len > 0) {
        RtcpCommonHeader* header = (RtcpCommonHeader*)p;
        uint16_t payload_length = GetRtcpLength(header);
        int item_total = (int)sizeof(RtcpCommonHeader) + payload_length;
        int ret = 0;

        LogDebugf(logger_, "rtcp type:%d, left_len:%d, item_total:%d", header->packet_type, left_len, item_total);
        switch (header->packet_type)
        {
            case RTCP_SR:
                {
                    ret = HandleRtcpSr(p, item_total);
                    break;
                }
            case RTCP_RR:
                {
                    ret = HandleRtcpRr(p, item_total);
                    break;
                }
            case RTCP_SDES:
                {
                    ret = left_len;
                    //rtcp sdes packet needn't to be handled.
                    break;
                }
            case RTCP_BYE:
                {
                    ret = left_len;
                    break;
                }
            case RTCP_APP:
                {
                    ret = left_len;
                    break;
                }
            case RTCP_RTPFB:
                {
                    ret = HandleRtcpRtpFb(p, item_total);
                    break;
                }
            case RTCP_PSFB:
                {
                    ret = HandleRtcpPsFb(p, item_total);
                    break;
                }
            case RTCP_XR:
                {
                    ret = HandleRtcpXr(p, item_total);
                    break;
                }
            default:
                {
                    LogErrorf(logger_, "unkown rtcp type:%d", header->packet_type);
                    ret = left_len;
                }
        }
        p        += ret;
        left_len -= ret;
    }

    return;
}

void PeerConnection::OnWrite(size_t sent_size, UdpTuple address) {
    return;
}

void PeerConnection::OnRead(const char* data, size_t data_size, UdpTuple address) {
    if (StunPacket::IsStun((uint8_t*)data, data_size)) {
        try {
            StunPacket* pkt = StunPacket::Parse((uint8_t*)data, data_size);
            if (pkt) {
                uint16_t port = 0;
                std::string ip = GetIpStr(pkt->xor_address_, port);
                if ((pc_udp_port_ == 0) || pc_ipaddr_str_.empty()
                    || (ip != pc_ipaddr_str_) || (port != pc_udp_port_)) {
                    LogInfof(logger_, "stun response update pc ipaddr:%s:%d", ip.c_str(), port);
                    pc_ipaddr_str_ = ip;
                    pc_udp_port_ = port;
                }
                delete pkt;
            }
            //LogInfof(logger_, "receive stun packet:%s", pkt->Dump().c_str());
            if (pc_state_ < PC_STUN_DONE_STATE) {
                pc_state_ = PC_STUN_DONE_STATE;
                Report("stun", "ready");
                dtls_.Start();
            }
        } catch(CppStreamException& e) {
            LogErrorf(logger_, "handle stun packet exception:%s", e.what());
            return;
        }
    } else if (IsRtcp((uint8_t*)data, data_size)) {
        HandleRtcp((uint8_t*)data, data_size);
    } else if (IsRtp((uint8_t*)data, data_size)) {
        HandleRtpData((uint8_t*)data, data_size);
    } else if (RtcDtls::IsDtls((uint8_t*)data, data_size)) {
        LogInfof(logger_, "receive dtls data size:%u", data_size);
        int dtls_len = (int)data_size;
        uint8_t dtls_data[8 * 1024];
        memcpy(dtls_data, data, data_size);
        dtls_.OnDtlsData(dtls_data, dtls_len);
        return;
    }

    udp_client_->TryRead();
}

void PeerConnection::HandleRtpData(uint8_t* data, size_t len) {
    if (pc_state_ < PC_DTLS_DONE_STATE) {
        return;
    }
    try {
        if (!read_srtp_) {
            LogErrorf(logger_, "read srtp session is not ready and discard rtp packet");
            return;
        }

        bool ret = read_srtp_->DecryptSrtp(const_cast<uint8_t*>(data), &len);
        if (!ret) {
            LogErrorf(logger_, "decrypt srtp error");
            return;
        }

        RtpPacket* pkt = RtpPacket::Parse(data, len);
        if (!pkt) {
            return;
        }
        uint32_t ssrc = pkt->GetSsrc();
        if (mspull_ && ssrc == 1234) {
            return;//discard ssrc=1234 which is test ssrc.
        }
        if (video_recv_stream_ && (ssrc == video_recv_stream_->GetSsrc() || ssrc == video_recv_stream_->GetRtxSsrc())) {
            video_recv_stream_->HandleRtpPacket(pkt);
            jb_video_.InputRtpPacket(video_recv_stream_->GetClockRate(), pkt);
        } else if (audio_recv_stream_ && ssrc == audio_recv_stream_->GetSsrc()) {
            audio_recv_stream_->HandleRtpPacket(pkt);
            jb_audio_.InputRtpPacket(audio_recv_stream_->GetClockRate(), pkt);
        } else {
            uint32_t v_ssrc   = video_recv_stream_ ? video_recv_stream_->GetSsrc() : 0;
            uint32_t rtx_ssrc = video_recv_stream_ ? video_recv_stream_->GetRtxSsrc() : 0;
            uint32_t a_ssrc   = audio_recv_stream_ ? audio_recv_stream_->GetSsrc() : 0;
            LogErrorf(logger_, "fail to find ssrc:%u, video ssrc:%u, video rtx ssrc:%u, audio ssrc:%u", 
                    ssrc, v_ssrc, rtx_ssrc, a_ssrc);
        }
    } catch(CppStreamException& e) {
        LogErrorf(logger_, "handle rtp data exception:%s", e.what());
    }
}

void PeerConnection::Report(const std::string& type, const std::string& value) {
    if (state_report_) {
        state_report_->OnState(type, value);
    }
}

void PeerConnection::OnDtlsConnected(CRYPTO_SUITE_ENUM suite,
                uint8_t* local_key, size_t local_key_len,
                uint8_t* remote_key, size_t remote_key_len) {
    LogInfoData(logger_, local_key, local_key_len, "on dtls connected srtp local key");
    LogInfoData(logger_, remote_key, remote_key_len, "on dtls connected srtp remote key");

    if (pc_state_ == PC_DTLS_DONE_STATE) {
        LogInfof(logger_, "dtls is connected.");
        return;
    }
    pc_state_ = PC_DTLS_DONE_STATE;

    try {
        Report("dtls", "ready");
        write_srtp_ = std::make_shared<SRtpSession>(SRTP_SESSION_OUT_TYPE, suite, local_key, local_key_len);
        read_srtp_  = std::make_shared<SRtpSession>(SRTP_SESSION_IN_TYPE, suite, remote_key, remote_key_len);

        if (direct_type_ == SEND_ONLY) {
            CreateSendStream();
        } else if (direct_type_ == RECV_ONLY) {
            if (!mspull_) {
                CreateRecvStream();
            }
        } else {
            LogErrorf(logger_, "peer connection direction type error:%d", direct_type_);
        }
    } catch(CppStreamException& e) {
        LogErrorf(logger_, "create srtp session error:%s", e.what());
    }
    return;
}

void PeerConnection::SendRtpPacket(uint8_t* data, size_t len) {
    if (!write_srtp_) {
        LogErrorf(logger_, "write_srtp is not ready");
        return;
    }
    
    bool ret = write_srtp_->EncryptRtp(const_cast<uint8_t**>(&data), &len);
    if (!ret) {
        LogErrorf(logger_, "encrypt_rtp error");
        return;
    }
    udp_client_->Write((char*)data, len, dtls_.remote_address_);
    udp_client_->TryRead();
}

void PeerConnection::SendRtcpPacket(uint8_t* data, size_t len) {
    if (!write_srtp_) {
        return;
    }
    bool ret = write_srtp_->EncryptRtcp(const_cast<uint8_t**>(&data), &len);
    if (!ret) {
        LogErrorf(logger_, "encrypt rtcp error");
        return;
    }
    udp_client_->Write((char*)data, len, dtls_.remote_address_);
    udp_client_->TryRead();
}

void PeerConnection::CreateVideoRecvStream() {
    bool video_nack = answer_sdp_.IsVideoNackEnable();
    uint32_t ssrc = answer_sdp_.GetVideoSsrc();
    int pt = answer_sdp_.GetVideoPayloadType();
    uint32_t rtx_ssrc    = answer_sdp_.GetVideoRtxSsrc();
    uint8_t rtx_pt  = answer_sdp_.GetVideoRtxPayloadType();
    int clock_rate = answer_sdp_.GetVideoClockRate();
    
    has_rtx_ = answer_sdp_.IsVideoRtxEnable();

    LogInfof(logger_, "create recv stream video ssrc:%u@%d%s, rtx %d ssrc:%u@%d, clock rate:%d",
            ssrc, pt, video_nack ? " nack" : "",
            has_rtx_, rtx_ssrc, rtx_pt, clock_rate);
    if (ssrc > 0) {
        video_recv_stream_ = std::make_shared<RtcRecvStream>(MEDIA_VIDEO_TYPE,
            ssrc, pt, clock_rate, video_nack,
            this, logger_, loop_);
        if (has_rtx_ && (rtx_pt > 0) && (rtx_ssrc > 0)) {
            video_recv_stream_->SetRtx(rtx_pt, rtx_ssrc);  
        }
        video_recv_stream_->RequestKeyFrame(-1);
    }
}

void PeerConnection::CreateAudioRecvStream() {
    bool audio_nack      = answer_sdp_.IsAudioNackEnable();
    uint32_t ssrc        = answer_sdp_.GetAudioSsrc();
    int payload_type     = answer_sdp_.GetAudioPayloadType();
    int clock_rate       = answer_sdp_.GetAudioClockRate();

    LogInfof(logger_, "create recv stream audio ssrc:%u@%d%s, clock rate:%d",
            ssrc, payload_type, audio_nack ? " nack" : "", clock_rate);

    if (answer_sdp_.GetAudioSsrc() > 0) {
        audio_recv_stream_ = std::make_shared<RtcRecvStream>(MEDIA_AUDIO_TYPE,
                ssrc, payload_type, clock_rate, audio_nack, 
                this, logger_, loop_);
        audio_recv_stream_->SetChannel(answer_sdp_.channel_);
    }
}

void PeerConnection::CreateRecvStream() {
    CreateVideoRecvStream();
    CreateAudioRecvStream();
}

void PeerConnection::CreateSendStream() {
    bool video_nack = answer_sdp_.IsVideoNackEnable();
    bool audio_nack = answer_sdp_.IsAudioNackEnable();

    LogInfof(logger_, "create send stream audio ssrc:%u, video ssrc:%u, rtx %d ssrc:%u@%d",
        answer_sdp_.GetAudioSsrc(), answer_sdp_.GetVideoSsrc(),
        answer_sdp_.IsVideoRtxEnable(), answer_sdp_.GetVideoRtxSsrc(), answer_sdp_.GetVideoRtxPayloadType());
    if (answer_sdp_.GetVideoSsrc() > 0) {
        video_send_stream_ = std::make_shared<RtcSendStream>(MEDIA_VIDEO_TYPE,
            answer_sdp_.GetVideoSsrc(),
            answer_sdp_.GetVideoPayloadType(),
            answer_sdp_.GetVideoClockRate(),
            video_nack, this, logger_);
        has_rtx_ = answer_sdp_.IsVideoRtxEnable();
        uint32_t rtx_ssrc    = answer_sdp_.GetVideoRtxSsrc();
        uint8_t rtx_payload  = answer_sdp_.GetVideoRtxPayloadType();
        if (has_rtx_ && (rtx_payload > 0) && (rtx_ssrc > 0)) {
            video_send_stream_->SetRtx(rtx_payload, rtx_ssrc);
        }
    }

    if (answer_sdp_.GetAudioSsrc() > 0) {
        audio_send_stream_ = std::make_shared<RtcSendStream>(MEDIA_AUDIO_TYPE,
                answer_sdp_.GetAudioSsrc(),
                answer_sdp_.GetAudioPayloadType(),
                answer_sdp_.GetAudioClockRate(),
                audio_nack, this, logger_);
        audio_send_stream_->SetChannel(answer_sdp_.channel_);
    }
}

void PeerConnection::CreateSendStream2() {
    bool video_nack = offer_sdp_.IsVideoNackEnable();
    bool audio_nack = offer_sdp_.IsAudioNackEnable();

    LogInfof(logger_, "create send stream audio ssrc:%u, video ssrc:%u, rtx %d ssrc:%u@%d",
        answer_sdp_.GetAudioSsrc(), answer_sdp_.GetVideoSsrc(),
        answer_sdp_.IsVideoRtxEnable(), answer_sdp_.GetVideoRtxSsrc(), answer_sdp_.GetVideoRtxPayloadType());

    if (offer_sdp_.GetVideoSsrc() > 0) {
        video_send_stream_ = std::make_shared<RtcSendStream>(MEDIA_VIDEO_TYPE,
            offer_sdp_.GetVideoSsrc(),
            offer_sdp_.GetVideoPayloadType(),
            offer_sdp_.GetVideoClockRate(),
            video_nack, this, logger_);
        has_rtx_ = offer_sdp_.IsVideoRtxEnable();
        uint32_t rtx_ssrc    = offer_sdp_.GetVideoRtxSsrc();
        uint8_t rtx_payload  = offer_sdp_.GetVideoRtxPayloadType();
        if (has_rtx_ && (rtx_payload > 0) && (rtx_ssrc > 0)) {
            video_send_stream_->SetRtx(rtx_payload, rtx_ssrc);
        }
    }

    if (offer_sdp_.GetAudioSsrc() > 0) {
        audio_send_stream_ = std::make_shared<RtcSendStream>(MEDIA_AUDIO_TYPE,
                offer_sdp_.GetAudioSsrc(),
                offer_sdp_.GetAudioPayloadType(),
                offer_sdp_.GetAudioClockRate(),
                audio_nack, this, logger_);
        audio_send_stream_->SetChannel(offer_sdp_.channel_);
    }
}

int PeerConnection::SendVideoPacket(Media_Packet_Ptr pkt_ptr) {
    if (!video_send_stream_) {
        return -1;
    }

    video_send_stream_->SendPacket(pkt_ptr);
    return 0;
}

int PeerConnection::SendAudioPacket(Media_Packet_Ptr pkt_ptr) {
    if (!audio_send_stream_) {
        return -1;
    }
    audio_send_stream_->SendPacket(pkt_ptr);
    return 0;
}

void PeerConnection::OnTimer() {
    if (pc_state_ < PC_SDP_DONE_STATE) {
        return;
    }

    int64_t now_ms = now_millisec();
    SendStun(now_ms);

    if (pc_state_ < PC_DTLS_DONE_STATE) {
        return;
    }

    if (video_send_stream_) {
        video_send_stream_->OnTimer(now_ms);
    }

    if (audio_send_stream_) {
        audio_send_stream_->OnTimer(now_ms);
    }

    if (video_recv_stream_) {
        video_recv_stream_->OnTimer(now_ms);
    }

    SendRr(now_ms);
    SendXrDlrr(now_ms);

    OnStatics(now_ms);
}

void PeerConnection::SendRr(int64_t now_ms) {
    RtcpRrPacket rr_pkt;
    RtcpRrBlockInfo* video_rr_block = nullptr;
    RtcpRrBlockInfo* audio_rr_block = nullptr;

    if (last_rr_ms_ <= 0) {
        last_rr_ms_ = now_ms;
        return;
    }
    int64_t diff_t = now_ms - last_rr_ms_;
    if (diff_t < RTCP_RR_INTERVAL) {
        return;
    }

    last_rr_ms_ = now_ms;
    if (video_recv_stream_) {
        video_rr_block = video_recv_stream_->GetRtcpRr(now_ms);
        if (video_rr_block) {
            rr_pkt.AddRrBlock(video_rr_block->GetBlock());
        }
    }
    if (audio_recv_stream_) {
        audio_rr_block = audio_recv_stream_->GetRtcpRr(now_ms);
        if (audio_rr_block) {
            rr_pkt.AddRrBlock(audio_rr_block->GetBlock());
        }
    }

    if (video_rr_block || audio_rr_block) {
        size_t data_len;
        uint8_t* data = rr_pkt.GetData(data_len);
        //LogInfof(logger_, "rtcp rr packet:%s", rr_pkt.Dump().c_str());
        SendRtcpPacket(data, data_len);
    }

    if (video_rr_block) {
        delete video_rr_block;
        video_rr_block = nullptr;
    }

    if (audio_rr_block) {
        delete audio_rr_block;
        audio_rr_block = nullptr;
    }
}

void PeerConnection::SendXrDlrr(int64_t now_ms) {
    XrDlrr dlrr_obj;

    if (direct_type_ != SEND_ONLY) {
        return;
    }
    if (last_send_xr_dlrr_ms_ <= 0) {
        last_send_xr_dlrr_ms_ = now_ms;
        return;
    }

    if (now_ms - last_send_xr_dlrr_ms_ < 1000) {
        return;
    }

    last_send_xr_dlrr_ms_ = now_ms;

    if (last_xr_ms_ <= 0) {
        return;
    }

    if ((now_ms - last_xr_ms_) > 5000) {
        return;
    }

    dlrr_obj.SetSsrc(0x01);
    
    uint32_t lrr = (last_xr_ntp_.ntp_sec & 0xffff) << 16;
    lrr |= (last_xr_ntp_.ntp_frac & 0xffff0000) >> 16;

    int64_t diff_ms = now_ms - last_xr_ms_;
    uint32_t dlrr = (uint32_t)(diff_ms / 1000) << 16;
    dlrr |= (uint32_t)((diff_ms % 1000) * 65536 / 1000);

    LogDebugf(logger_, "send xr dlrr lrr:%u, dlrr:%u", lrr, dlrr);

    if (video_send_stream_) {
        uint32_t rtp_ssrc = video_send_stream_->GetSsrc();
        dlrr_obj.AddrDlrrBlock(rtp_ssrc, lrr, dlrr);
    }

    if (audio_send_stream_) {
        uint32_t rtp_ssrc = audio_send_stream_->GetSsrc();
        dlrr_obj.AddrDlrrBlock(rtp_ssrc, lrr, dlrr);
    }

    SendRtcpPacket(dlrr_obj.GetData(), dlrr_obj.GetDataLen());
}

void PeerConnection::OnStatics(int64_t now_ms) {
    if (pc_state_ < PC_DTLS_DONE_STATE) {
        return;
    }

    if (last_statics_ms_ <= 0) {
        last_statics_ms_ = now_ms;
        return;
    }

    if (last_statics_ms_ + 2000 > now_ms) {
        return;
    }
    last_statics_ms_ = now_ms;

    if (video_send_stream_) {
        size_t vkps = 0;
        size_t vpps = 0;
        int64_t resend_pps = 0;
        int64_t resend_total = 0;
        std::stringstream ss;
        
        resend_total = video_send_stream_->GetResendCount(now_ms, resend_pps);
        video_send_stream_->GetStatics(vkps, vpps);
        ss << "{";
        ss << "\"v_kbps\":" << vkps << ","; 
        ss << "\"v_pps\":" << vpps << ",";
        ss << "\"rtt\":" << video_send_stream_->GetRtt() << ",";
        ss << "\"jitter\":" << video_send_stream_->GetJitter() << ",";
        ss << "\"lost\":" << video_send_stream_->GetLostRate() << ",";
        ss << "\"resend total\":" << resend_total << ",";
        ss << "\"resend pps\":" << resend_pps;
        ss << "}";
        Report("video_statics", ss.str());
    }

    if (audio_send_stream_) {
        size_t akps = 0;
        size_t apps = 0;
        std::stringstream ss;
        audio_send_stream_->GetStatics(akps, apps);
        ss << "{";
        ss << "\"a_kbps\":" << akps << ","; 
        ss << "\"a_pps\":" << apps;
        ss << "\"rtt\":" << audio_send_stream_->GetRtt() << ",";
        ss << "\"jitter\":" << audio_send_stream_->GetJitter() << ",";
        ss << "\"lost\":" << audio_send_stream_->GetLostRate();
        ss << "}";
        Report("audio_statics", ss.str());
    }

    if (video_recv_stream_) {
        size_t vkps = 0;
        size_t vpps = 0;
        int64_t resend_pps = 0;
        int64_t resend_total = 0;
        std::stringstream ss;
        
        resend_total = video_recv_stream_->GetResendCount(now_ms, resend_pps);
        video_recv_stream_->GetStatics(vkps, vpps);
        ss << "{";
        ss << "\"v_kbps\":" << vkps << ","; 
        ss << "\"v_pps\":" << vpps << ",";
        ss << "\"rtt\":" << video_recv_stream_->GetRtt() << ",";
        ss << "\"jitter\":" << video_recv_stream_->GetJitter() << ",";
        ss << "\"lost\":" << video_recv_stream_->GetLostRate() << ",";
        ss << "\"resend total\":" << resend_total << ",";
        ss << "\"resend pps\":" << resend_pps;
        ss << "}";
        Report("video_statics", ss.str());
    }

    if (audio_recv_stream_) {
        size_t vkps = 0;
        size_t vpps = 0;
        std::stringstream ss;

        audio_recv_stream_->GetStatics(vkps, vpps);
        ss << "{";
        ss << "\"a_kbps\":" << vkps << ","; 
        ss << "\"a_pps\":" << vpps << ",";
        ss << "\"rtt\":" << audio_recv_stream_->GetRtt() << ",";
        ss << "\"jitter\":" << audio_recv_stream_->GetJitter() << ",";
        ss << "\"lost\":" << audio_recv_stream_->GetLostRate();
        ss << "}";
        Report("audio_statics", ss.str());
    }

}

void PeerConnection::SetRemoteIcePwd(const std::string& ice_pwd) {
    dtls_.remote_pwd_ = ice_pwd;
}

void PeerConnection::SetRemoteIceUserFrag(const std::string& user_frag) {
    dtls_.remote_fragment_ = user_frag;
}

void PeerConnection::SetRemoteUdpAddress(const std::string& ip, uint16_t port) {
    dtls_.remote_address_.ip_address = ip;
    dtls_.remote_address_.port = port;

    IceInfo ice_info;
    ice_info.net_type = ICE_UDP;
    ice_info.hostip   = ip;
    ice_info.port     = port;

    if (dtls_.ice_infos.empty()) {
        dtls_.ice_infos.push_back(ice_info);
    } else {
        bool found = false;
        for (auto& item : dtls_.ice_infos) {
            if (item == ice_info) {
                found = true;
                break;
            }
        }
        if (!found) {
            dtls_.ice_infos.push_back(ice_info);
        }
    }
}

void PeerConnection::SetFingerPrintsSha256(const std::string& sha256_value) {

}

std::string PeerConnection::GetFingerSha256() {
    return dtls_.fingerprint_;
}

void PeerConnection::UpdatePcState(PC_STATE pc_state) {
    pc_state_ = pc_state;
}

void PeerConnection::SetVideoMid(SDP_TYPE type, int mid) {
    if (type == SDP_OFFER) {
        offer_sdp_.vmid_ = mid;
    } else {
        answer_sdp_.vmid_ = mid;
    }
}

void PeerConnection::SetAudioMid(SDP_TYPE type, int mid) {
    if (type == SDP_OFFER) {
        offer_sdp_.amid_ = mid;
    } else {
        answer_sdp_.amid_ = mid;
    }
}

void PeerConnection::SetVideoSsrc(SDP_TYPE type, uint32_t ssrc) {
    if (type == SDP_OFFER) {
        SSRCInfo video_ssrc_info = {};
        video_ssrc_info.ssrc         = ssrc;
        video_ssrc_info.is_video     = true;
        video_ssrc_info.is_rtx       = false;
        video_ssrc_info.msid         = offer_sdp_.v_msid_;
        video_ssrc_info.msid_appdata = offer_sdp_.v_msid_appdata_;
        video_ssrc_info.cname        = offer_sdp_.video_cname_;
        video_ssrc_info.rtx_ssrc     = offer_sdp_.video_rtx_ssrc_;
        offer_sdp_.ssrc_info_map_[ssrc] = video_ssrc_info;

        offer_sdp_.video_ssrc_ = ssrc;
    } else {
        SSRCInfo video_ssrc_info = {};
        video_ssrc_info.ssrc = ssrc;
        video_ssrc_info.is_video     = true;
        video_ssrc_info.is_rtx       = false;
        video_ssrc_info.msid         = answer_sdp_.v_msid_;
        video_ssrc_info.msid_appdata = answer_sdp_.v_msid_appdata_;
        video_ssrc_info.cname        = answer_sdp_.video_cname_;
        video_ssrc_info.rtx_ssrc     = answer_sdp_.video_rtx_ssrc_;
        answer_sdp_.ssrc_info_map_[ssrc] = video_ssrc_info;

        answer_sdp_.video_ssrc_ = ssrc;
    }
}

void PeerConnection::SetVideoRtxSsrc(SDP_TYPE type, uint32_t ssrc) {
    if (type == SDP_OFFER) {
        SSRCInfo rtx_ssrc_info = {};
        rtx_ssrc_info.ssrc         = ssrc,
        rtx_ssrc_info.is_video     = true,
        rtx_ssrc_info.is_rtx       = true,
        rtx_ssrc_info.msid         = offer_sdp_.v_msid_,
        rtx_ssrc_info.msid_appdata = offer_sdp_.v_msid_appdata_,
        rtx_ssrc_info.cname        = offer_sdp_.video_cname_,
        rtx_ssrc_info.rtx_ssrc     = offer_sdp_.video_ssrc_;
        offer_sdp_.ssrc_info_map_[ssrc] = rtx_ssrc_info;
        offer_sdp_.video_rtx_ssrc_ = ssrc;
    } else {
        SSRCInfo rtx_ssrc_info = {};
        rtx_ssrc_info.ssrc         = ssrc,
        rtx_ssrc_info.is_video     = true,
        rtx_ssrc_info.is_rtx       = true,
        rtx_ssrc_info.msid         = answer_sdp_.v_msid_,
        rtx_ssrc_info.msid_appdata = answer_sdp_.v_msid_appdata_,
        rtx_ssrc_info.cname        = answer_sdp_.video_cname_,
        rtx_ssrc_info.rtx_ssrc     = answer_sdp_.video_ssrc_;
        answer_sdp_.ssrc_info_map_[ssrc] = rtx_ssrc_info;
        answer_sdp_.video_rtx_ssrc_ = ssrc;
    }
}

void PeerConnection::SetAudioSsrc(SDP_TYPE type, uint32_t ssrc) {
    if (type == SDP_OFFER) {
        SSRCInfo audio_ssrc_info = {};
        audio_ssrc_info.ssrc         = ssrc;
        audio_ssrc_info.is_video     = false;
        audio_ssrc_info.is_rtx       = false;
        audio_ssrc_info.msid         = offer_sdp_.a_msid_;
        audio_ssrc_info.msid_appdata = offer_sdp_.a_msid_appdata_;
        audio_ssrc_info.cname        = offer_sdp_.audio_cname_;
        audio_ssrc_info.rtx_ssrc     = 0;
        offer_sdp_.ssrc_info_map_[ssrc] = audio_ssrc_info;
        offer_sdp_.audio_ssrc_ = ssrc;
    } else {
        SSRCInfo audio_ssrc_info = {};
        audio_ssrc_info.ssrc         = ssrc,
        audio_ssrc_info.is_video     = false,
        audio_ssrc_info.is_rtx       = false,
        audio_ssrc_info.msid         = answer_sdp_.a_msid_,
        audio_ssrc_info.msid_appdata = answer_sdp_.a_msid_appdata_,
        audio_ssrc_info.cname        = answer_sdp_.audio_cname_,
        audio_ssrc_info.rtx_ssrc     = 0;
        answer_sdp_.ssrc_info_map_[ssrc] = audio_ssrc_info;
        answer_sdp_.audio_ssrc_ = ssrc;
    }
}

bool PeerConnection::GetVideoRtx(SDP_TYPE type) {
    if (type == SDP_OFFER) {
        auto iter = offer_sdp_.ssrc_info_map_.find(offer_sdp_.video_rtx_ssrc_);
        if (iter == offer_sdp_.ssrc_info_map_.end()) {
            CSM_THROW_ERROR("offer video rtx ssrc is not initied");
        }
        return iter->second.is_rtx;
    } else {
        auto iter = answer_sdp_.ssrc_info_map_.find(answer_sdp_.video_rtx_ssrc_);
        if (iter == answer_sdp_.ssrc_info_map_.end()) {
            CSM_THROW_ERROR("answer video rtx ssrc is not initied");
        }
        return iter->second.is_rtx;
    }
}

int PeerConnection::GetVideoPayloadType(SDP_TYPE type) {
    if (type == SDP_OFFER) {
        for(auto& rtp_item : offer_sdp_.video_rtp_map_infos_) {
            if (rtp_item.second.codec_type != "rtx") {
                return rtp_item.first;
            }
        }
    } else {
        for(auto& rtp_item : answer_sdp_.video_rtp_map_infos_) {
            if (rtp_item.second.codec_type != "rtx") {
                return rtp_item.first;
            }
        }
    }
    return -1;
}

void PeerConnection::SetVideoPayloadType(SDP_TYPE type, int payloadType) {
    RtpMapInfo info;
    info.payload_type = payloadType;
    info.codec_type   = "H264";
    info.clock_rate   = 90000;

    if (type == SDP_OFFER) {
        offer_sdp_.video_rtp_map_infos_[payloadType] = info;
    } else {
        answer_sdp_.video_rtp_map_infos_[payloadType] = info;
    }
}

int PeerConnection::GetVideoRtxPayloadType(SDP_TYPE type) {
    if (type == SDP_OFFER) {
        for(auto& rtp_item : offer_sdp_.video_rtp_map_infos_) {
            if (rtp_item.second.codec_type == "rtx") {
                return rtp_item.first;
            }
        }
    } else {
        for(auto& rtp_item : answer_sdp_.video_rtp_map_infos_) {
            if (rtp_item.second.codec_type == "rtx") {
                return rtp_item.first;
            }
        }
    }
    return -1;
}

void PeerConnection::SetVideoRtxPayloadType(SDP_TYPE type, int payloadType) {
    RtpMapInfo info;
    info.payload_type = payloadType;
    info.codec_type   = "rtx";
    info.clock_rate   = 90000;

    FmtpInfo fmtp_info;
    fmtp_info.payload_type = GetVideoPayloadType(type);
    fmtp_info.attr_string  = "apt=";
    fmtp_info.is_video     = true;
    fmtp_info.is_rtx       = true;
    fmtp_info.rtx_payload_type = payloadType;

    if (type == SDP_OFFER) {
        offer_sdp_.video_rtp_map_infos_[payloadType] = info;
        offer_sdp_.video_fmtp_vec_.push_back(fmtp_info);
    } else {
        answer_sdp_.video_rtp_map_infos_[payloadType] = info;
        answer_sdp_.video_fmtp_vec_.push_back(fmtp_info);
    }
}

int PeerConnection::GetAudioPayloadType(SDP_TYPE type) {
    if (type == SDP_OFFER) {
        for(auto& rtp_item : offer_sdp_.audio_rtp_map_infos_) {
            if (rtp_item.second.codec_type != "rtx") {
                return rtp_item.first;
            }
        }
    } else {
        for(auto& rtp_item : answer_sdp_.audio_rtp_map_infos_) {
            if (rtp_item.second.codec_type != "rtx") {
                return rtp_item.first;
            }
        }
    }
    return -1;
}

void PeerConnection::SetAudioPayloadType(SDP_TYPE type, int payloadType) {
    RtpMapInfo opusRtpInfo = {payloadType, "opus", 48000, 2};
    if (type == SDP_OFFER) {
        offer_sdp_.audio_rtp_map_infos_[payloadType] = opusRtpInfo;
    } else {
        answer_sdp_.audio_rtp_map_infos_[payloadType] = opusRtpInfo;
    }
}

std::string PeerConnection::GetVideoCodecType(SDP_TYPE type) {
    if (type == SDP_OFFER) {
        for(auto& rtp_item : offer_sdp_.video_rtp_map_infos_) {
            if (rtp_item.second.codec_type != "rtx") {
                return rtp_item.second.codec_type;
            }
        }
    } else {
        for(auto& rtp_item : answer_sdp_.video_rtp_map_infos_) {
            if (rtp_item.second.codec_type != "rtx") {
                return rtp_item.second.codec_type;
            }
        }
    }
    return "";
}

std::string PeerConnection::GetAudioCodecType(SDP_TYPE type) {
    if (type == SDP_OFFER) {
        for(auto& rtp_item : offer_sdp_.audio_rtp_map_infos_) {
            return rtp_item.second.codec_type;
        }
    } else {
        for(auto& rtp_item : answer_sdp_.audio_rtp_map_infos_) {
            return rtp_item.second.codec_type;
        }
    }

    return "";
}

int PeerConnection::GetVideoClockRate(SDP_TYPE type) {
    if (type == SDP_OFFER) {
        for(auto& rtp_item : offer_sdp_.video_rtp_map_infos_) {
            if (rtp_item.second.codec_type != "rtx") {
                return rtp_item.second.clock_rate;
            }
        }
    } else {
        for(auto& rtp_item : answer_sdp_.video_rtp_map_infos_) {
            if (rtp_item.second.codec_type != "rtx") {
                return rtp_item.second.clock_rate;
            }
        }
    }
    return -1;
}

void PeerConnection::SetVideoClockRate(SDP_TYPE type, int clock_rate) {
    if (type == SDP_OFFER) {
        for(auto& rtp_item : offer_sdp_.video_rtp_map_infos_) {
            if (rtp_item.second.codec_type != "rtx") {
                rtp_item.second.clock_rate = clock_rate;
            }
        }
    } else {
        for(auto& rtp_item : answer_sdp_.video_rtp_map_infos_) {
            if (rtp_item.second.codec_type != "rtx") {
                rtp_item.second.clock_rate = clock_rate;
            }
        }
    }
}

int PeerConnection::GetAudioClockRate(SDP_TYPE type) {
    if (type == SDP_OFFER) {
        for(auto& rtp_item : offer_sdp_.audio_rtp_map_infos_) {
            return rtp_item.second.clock_rate;
        }
    } else {
        for(auto& rtp_item : answer_sdp_.audio_rtp_map_infos_) {
            return rtp_item.second.clock_rate;
        }
    }
    return -1;
}


void PeerConnection::SetAudioClockRate(SDP_TYPE type, int clock_rate) {
    if (type == SDP_OFFER) {
        for(auto& rtp_item : offer_sdp_.audio_rtp_map_infos_) {
            rtp_item.second.clock_rate = clock_rate;
        }
    } else {
        for(auto& rtp_item : answer_sdp_.audio_rtp_map_infos_) {
            rtp_item.second.clock_rate = clock_rate;
        }
    }
}

void PeerConnection::SetVideoNack(SDP_TYPE type, bool enable, int payload_type) {
    if (type == SDP_OFFER) {
        bool found = findRtcpFb(offer_sdp_.video_rtcpfb_vec_, "nack", payload_type);
        if (!found) {
            RtcpFbInfo info;
            info.payload_type = payload_type;
            info.attr_string  = "nack";
            offer_sdp_.video_rtcpfb_vec_.push_back(info);
        }
    } else {
        bool found = findRtcpFb(answer_sdp_.video_rtcpfb_vec_, "nack", payload_type);
        if (!found) {
            RtcpFbInfo info;
            info.payload_type = payload_type;
            info.attr_string  = "nack";
            answer_sdp_.video_rtcpfb_vec_.push_back(info);
        }
    }
}

bool PeerConnection::GetVideoNack(SDP_TYPE type, int payload_type) {
    if (type == SDP_OFFER) {
        return findRtcpFb(offer_sdp_.video_rtcpfb_vec_, "nack", payload_type);
    } else {
        return findRtcpFb(answer_sdp_.video_rtcpfb_vec_, "nack", payload_type);
    }

}

void PeerConnection::SetCCType(SDP_TYPE type, CC_TYPE cc_type) {
    std::string cc_type_string = (cc_type == TCC_TYPE) ? "transport-cc" : "goog-remb";

    if (type == SDP_OFFER) {
        std::set<int> pt_set;
        auto iter = offer_sdp_.video_rtcpfb_vec_.begin();
        while(iter != offer_sdp_.video_rtcpfb_vec_.end()) {
            if (iter->attr_string == "transport-cc" && cc_type == TCC_TYPE) {
                pt_set.erase(iter->payload_type);
            } else if (iter->attr_string == "goog-remb" && cc_type == GCC_TYPE) {
                pt_set.erase(iter->payload_type);
            } else {
                pt_set.insert(iter->payload_type);
            }
            iter++;
        }
        for (auto &item : pt_set) {
            RtcpFbInfo info;
            info.payload_type = item;
            info.attr_string  = cc_type_string;
            offer_sdp_.video_rtcpfb_vec_.push_back(info);
        }
    } else {
        std::set<int> pt_set;
        auto iter = answer_sdp_.video_rtcpfb_vec_.begin();
        while(iter != answer_sdp_.video_rtcpfb_vec_.end()) {
            if (iter->attr_string == "transport-cc" && cc_type == TCC_TYPE) {
                pt_set.erase(iter->payload_type);
            } else if (iter->attr_string == "goog-remb" && cc_type == GCC_TYPE) {
                pt_set.erase(iter->payload_type);
            } else {
                pt_set.insert(iter->payload_type);
            }
            iter++;
        }
        for (auto &item : pt_set) {
            RtcpFbInfo info;
            info.payload_type = item;
            info.attr_string  = cc_type_string;
            answer_sdp_.video_rtcpfb_vec_.push_back(info);
        }
    }
}

CC_TYPE PeerConnection::GetCCType(SDP_TYPE type) {
    if (type == SDP_OFFER) {
        auto iter = offer_sdp_.video_rtcpfb_vec_.begin();
        while(iter != offer_sdp_.video_rtcpfb_vec_.end()) {
            if (iter->attr_string == "transport-cc") {
                return TCC_TYPE;
            } else if (iter->attr_string == "goog-remb") {
                return GCC_TYPE;
            }
            iter++;
        }
    } else {
        auto iter = answer_sdp_.video_rtcpfb_vec_.begin();
        while(iter != answer_sdp_.video_rtcpfb_vec_.end()) {
            if (iter->attr_string == "transport-cc") {
                return TCC_TYPE;
            } else if (iter->attr_string == "goog-remb") {
                return GCC_TYPE;
            }
            iter++;
        }
    }
    return CC_UNKNOWN_TYPE;
}

void PeerConnection::AddRtpExtInfo(SDP_TYPE type, int id, const RTP_EXT_INFO& info) {
    rtp_ext_headers_[id] = info;

    ExtInfo ext_info;
    ext_info.ext_id = id;
    ext_info.desc   = info.uri;
    if (type == SDP_OFFER) {
        offer_sdp_.ext_map_[id]  = ext_info;
    } else {
        answer_sdp_.ext_map_[id] = ext_info;
    }
}

void PeerConnection::GetHeaderExternId(int& offset_id, int& abs_send_time_id,
            int& video_rotation_id, int& tcc_id,
            int& playout_delay_id, int& video_content_id,
            int& video_timing_id, int& color_space_id,
            int& sdes_id, int& rtp_streamid_id,
            int& rp_rtp_streamid_id, int& audio_level) {
    offset_id          = offer_sdp_.em_toffset_;
    abs_send_time_id   = offer_sdp_.em_abs_send_time_;
    video_rotation_id  = offer_sdp_.em_video_rotation_;
    tcc_id             = offer_sdp_.em_tcc_;
    playout_delay_id   = offer_sdp_.em_playout_delay_;
    video_content_id   = offer_sdp_.em_video_content_;
    video_timing_id    = offer_sdp_.em_video_timing_;
    color_space_id     = offer_sdp_.em_color_space_;
    sdes_id            = offer_sdp_.em_sdes_;
    rtp_streamid_id    = offer_sdp_.em_rtp_streamid_;
    rp_rtp_streamid_id = offer_sdp_.em_rp_rtp_streamid_;
    audio_level        = offer_sdp_.em_audio_level_;
}

void PeerConnection::RtpPacketReset(std::shared_ptr<RtpPacketInfo> pkt_ptr) {
    LogInfof(logger_, "rtp jitter buffer reset");
}

void PeerConnection::RtpPacketOutput(std::shared_ptr<RtpPacketInfo> pkt_ptr) {
    if (pkt_ptr->media_type_ == MEDIA_VIDEO_TYPE) {
        if (!video_pack_) {
            std::string codec = GetVideoCodecType(SDP_ANSWER);
            auto pack = new PackHandleH264(this, loop_, logger_);
            if (codec == "H265") {
                pack->setH265(true);
            }
            video_pack_.reset(pack);
        }
        video_pack_->InputRtpPacket(pkt_ptr);
    } else if (pkt_ptr->media_type_ == MEDIA_AUDIO_TYPE) {
        if (!audio_pack_) {
            audio_pack_.reset(new PackHandleAudio(this, logger_));
        }
        audio_pack_->InputRtpPacket(pkt_ptr);
    } else {
        LogErrorf(logger_, "rtp pack receive unknown media type:%d", pkt_ptr->media_type_);
    }
}

void PeerConnection::PackHandleReset(std::shared_ptr<RtpPacketInfo> pkt_ptr) {
    LogInfof(logger_, "pack handle reset");
    find_keyframe_ = true;
    if (video_recv_stream_) {
        video_recv_stream_->RequestKeyFrame(-1);
    }
}

void PeerConnection::MediaPacketOutput(std::shared_ptr<Media_Packet> pkt_ptr) {
    if (pkt_ptr->av_type_ == MEDIA_VIDEO_TYPE) {
        pkt_ptr->dts_ = pkt_ptr->pts_ = pkt_ptr->dts_ * 1000 / video_recv_stream_->GetClockRate();
        uint8_t* p = (uint8_t*)pkt_ptr->buffer_ptr_->Data();
        int pos = GetNaluTypePos(p);
        if (H264_IS_AUD(p[pos])) {
            return;
        }
        if (find_keyframe_) {
            if (!H264_IS_KEYFRAME(p[pos])) {
                return;
            }
            find_keyframe_ = false;;
        }
    } else if (pkt_ptr->av_type_ == MEDIA_AUDIO_TYPE) {
        pkt_ptr->dts_ = pkt_ptr->pts_ = pkt_ptr->dts_ * 1000 / audio_recv_stream_->GetClockRate();
    } else {
        LogErrorf(logger_, "media packet unknown type:%d", pkt_ptr->av_type_);
        return;
    }
    if (media_cb_) {
        media_cb_->OnReceiveMediaPacket(pkt_ptr);
    }
}

}
