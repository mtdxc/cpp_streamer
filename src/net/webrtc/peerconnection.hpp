#ifndef PEER_CONNECTION_HPP
#define PEER_CONNECTION_HPP

#include "logger.hpp"
#include "dtls.hpp"
#include "sdp.hpp"
#include "udp_client.hpp"
#include "srtp_session.hpp"
#include "rtc_send_stream.hpp"
#include "rtc_recv_stream.hpp"
#include "jitterbuffer.hpp"
#include "timer.hpp"
#include "rtcp_xr_dlrr.hpp"
#include "pack_handle_pub.hpp"
#include "media_callback_interface.hpp"

namespace cpp_streamer
{
#define RTP_EXT_MAP(XX) \
    XX(MID_TYPE,                 "urn:ietf:params:rtp-hdrext:sdes:mid") \
    XX(RTP_STREAMID_TYPE,        "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id") \
    XX(RP_RTP_STREAMID_TYPE,     "urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id") \
    XX(ABS_SEND_TIME_TYPE,       "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time") \
    XX(TCC_WIDE_TYPE,            "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01") \
    XX(AVTEXT_FRAMEMARKING_TYPE, "http://tools.ietf.org/html/draft-ietf-avtext-framemarking-07") \
    XX(RTP_HDREXT_FRAMEMARKING_TYPE, "urn:ietf:params:rtp-hdrext:framemarking") \
    XX(SSRC_AUDIO_LEVEL_TYPE,    "urn:ietf:params:rtp-hdrext:ssrc-audio-level") \
    XX(VIDEO_ORIENTATION_TYPE,   "urn:3gpp:video-orientation") \
    XX(TOFFSET_TYPE,             "urn:ietf:params:rtp-hdrext:toffset") \
    XX(ABS_CAPTURE_TIME_TYPE,    "http://www.webrtc.org/experiments/rtp-hdrext/abs-capture-time") \
    XX(video_timing,             "http://www.webrtc.org/experiments/rtp-hdrext/video-timing") \
    XX(color_space,              "http://www.webrtc.org/experiments/rtp-hdrext/color-space") \
    XX(csrc_audio_level,         "urn:ietf:params:rtp-hdrext:csrc-audio-level") \
    XX(video_content_type,       "http://www.webrtc.org/experiments/rtp-hdrext/video-content-type") \
    XX(playout_delay,            "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay") \
    XX(encrypt,                  "urn:ietf:params:rtp-hdrext:encrypt")

enum RTP_EXT_TYPE {
    padding = 0,
#define XX(type, uri) type,
    RTP_EXT_MAP(XX)
#undef XX
    reserved = 15,
};
RTP_EXT_TYPE GetRtpExtType(const std::string& uri);
const char* RtpExtUri(RTP_EXT_TYPE ext);

typedef struct RTP_EXT_INFO_S {
    int id;
    RTP_EXT_TYPE type;
    std::string uri;
} RTP_EXT_INFO;

typedef enum {
    CC_UNKNOWN_TYPE,
    GCC_TYPE,
    TCC_TYPE
} CC_TYPE;

typedef enum {
    DIRECTION_UNKNOWN,
    SEND_ONLY,
    RECV_ONLY,
    SEND_RECV
} WebRtcSdpDirection;
const char* GetDirectionString(WebRtcSdpDirection dir);

typedef enum {
    PC_INIT_STATE,
    PC_SDP_DONE_STATE,
    PC_STUN_DONE_STATE,
    PC_DTLS_DONE_STATE
} PC_STATE;

typedef enum {
    SDP_OFFER,
    SDP_ANSWER
} SDP_TYPE;

class PCStateReportI
{
public:
    virtual void OnState(const std::string& type, const std::string& value) = 0;
};

class PeerConnection : public UdpSessionCallbackI
    , public RtcSendStreamCallbackI
    , public TimerInterface
    , public JitterBufferCallbackI
    , public PackCallbackI
{
public:
    PeerConnection(uv_loop_t* loop, Logger* logger, PCStateReportI* state_report);
    virtual ~PeerConnection();

public:
    std::string CreateOfferSdp(WebRtcSdpDirection dir);
    int ParseAnswerSdp(const std::string& sdp);
    int SendVideoPacket(Media_Packet_Ptr pkt_ptr);
    int SendAudioPacket(Media_Packet_Ptr pkt_ptr);

public:
    void SetRemoteIcePwd(const std::string& ice_pwd);
    void SetRemoteIceUserFrag(const std::string& user_frag);
    void SetRemoteUdpAddress(const std::string& ip, uint16_t port);
    void SetFingerPrintsSha256(const std::string& sha256_value);
    std::string GetFingerSha256();
    void UpdatePcState(PC_STATE pc_state);

public:
    int GetVideoMid(SDP_TYPE type) {
        return (type == SDP_OFFER) ? offer_sdp_.vmid_ : answer_sdp_.vmid_;
    }
    void SetVideoMid(SDP_TYPE type, int mid);

    int GetAudioMid(SDP_TYPE type) {
        return (type == SDP_OFFER) ? offer_sdp_.amid_ : answer_sdp_.amid_;
    }
    void SetAudioMid(SDP_TYPE type, int mid);

    void SetVideoSsrc(SDP_TYPE type, uint32_t ssrc);
    uint32_t GetVideoSsrc(SDP_TYPE type) {
        return (type == SDP_OFFER) ? offer_sdp_.video_ssrc_ : answer_sdp_.video_ssrc_;
    }

    void SetVideoRtxSsrc(SDP_TYPE type, uint32_t ssrc);
    uint32_t GetVideoRtxSsrc(SDP_TYPE type) {
        return (type == SDP_OFFER) ? offer_sdp_.video_rtx_ssrc_ : answer_sdp_.video_rtx_ssrc_;
    }

    void SetAudioSsrc(SDP_TYPE type, uint32_t ssrc);
    uint32_t GetAudioSsrc(SDP_TYPE type) {
        return (type == SDP_OFFER) ? offer_sdp_.audio_ssrc_ : answer_sdp_.audio_ssrc_;
    }

    void SetVideoPayloadType(SDP_TYPE type, int payloadType);
    int GetVideoPayloadType(SDP_TYPE type);

    void SetVideoRtxPayloadType(SDP_TYPE type, int payloadType);
    int GetVideoRtxPayloadType(SDP_TYPE type);

    void SetAudioPayloadType(SDP_TYPE type, int payloadType);
    int GetAudioPayloadType(SDP_TYPE type);

    std::string GetVideoCodecType(SDP_TYPE type);
    std::string GetAudioCodecType(SDP_TYPE type);

    bool GetVideoRtx(SDP_TYPE type);

    int GetVideoClockRate(SDP_TYPE type);
    void SetVideoClockRate(SDP_TYPE type, int clock_rate);

    int GetAudioClockRate(SDP_TYPE type);
    void SetAudioClockRate(SDP_TYPE type, int clock_rate);

    void SetVideoNack(SDP_TYPE type, bool enable, int payload_type);
    bool GetVideoNack(SDP_TYPE type, int payload_type);

    void SetCCType(SDP_TYPE type, CC_TYPE cc_type);
    CC_TYPE GetCCType(SDP_TYPE type);

    void AddRtpExtInfo(SDP_TYPE type, int id, const RTP_EXT_INFO& info);
    void GetHeaderExternId(int& offset_id, int& abs_send_time_id,
        int& video_rotation_id, int& tcc_id,
        int& playout_delay_id, int& video_content_id,
        int& video_timing_id, int& color_space_id,
        int& sdes_id, int& rtp_streamid_id,
        int& rp_rtp_streamid_id, int& audio_level);

    std::vector<RtcpFbInfo> GetVideoRtcpFbInfo() {
        return offer_sdp_.video_rtcpfb_vec_;
    }

    std::vector<RtcpFbInfo> GetAudioRtcpFbInfo() {
        return offer_sdp_.audio_rtcpfb_vec_;
    }

    std::string GetVideoCName() {
        return offer_sdp_.video_cname_;
    }

    std::string GetAudioCName() {
        return offer_sdp_.audio_cname_;
    }

    void CreateSendStream2();
    void CreateVideoRecvStream();
    void CreateAudioRecvStream();

public:
    void SetMediaCallback(MediaCallbackI* cb) { media_cb_ = cb; }

public:
    void OnDtlsConnected(CRYPTO_SUITE_ENUM suite,
                uint8_t* local_key, size_t local_key_len,
                uint8_t* remote_key, size_t remote_key_len);

//TimerInterface
protected:
    virtual void OnTimer() override;

protected:
    virtual void OnWrite(size_t sent_size, UdpTuple address) override;
    virtual void OnRead(const char* data, size_t data_size, UdpTuple address) override;

protected:
    virtual void SendRtpPacket(uint8_t* data, size_t len) override;
    virtual void SendRtcpPacket(uint8_t* data, size_t len) override;

public:
    virtual void RtpPacketReset(std::shared_ptr<RtpPacketInfo> pkt_ptr) override;
    virtual void RtpPacketOutput(std::shared_ptr<RtpPacketInfo> pkt_ptr) override;

public:
    virtual void PackHandleReset(std::shared_ptr<RtpPacketInfo> pkt_ptr) override;
    virtual void MediaPacketOutput(std::shared_ptr<Media_Packet> pkt_ptr) override;

public:
    void SetMsPull(bool enable) { mspull_ = enable; }
    bool GetMsPull() { return mspull_; }
private://for mediasoup pull
    bool mspull_ = false;

private:
    void Report(const std::string& key, const std::string& value);

private:
    void CreateSendStream();
    void CreateRecvStream();
    void OnStatics(int64_t now_ms);

private:
    void HandleRtpData(uint8_t* data, size_t len);

private:
    void HandleRtcp(uint8_t* data, size_t len);
    int HandleRtcpSr(uint8_t* data, int len);
    int HandleRtcpRr(uint8_t* data, int len);
    int HandleRtcpRtpFb(uint8_t* data, int len);
    int HandleRtcpPsFb(uint8_t* data, int len);
    int HandleRtcpXr(uint8_t* data, int len);
    int HandleXrDlrr(XrDlrrData* dlrr_block);

    void SendStun(int64_t now_ms);
    void SendXrDlrr(int64_t now_ms);
    void SendRr(int64_t now_ms);

private:
    uv_loop_t* loop_ = nullptr;
    Logger* logger_ = nullptr;
    PCStateReportI* state_report_   = nullptr;
    WebRtcSdpDirection direct_type_ = DIRECTION_UNKNOWN;
    PC_STATE pc_state_ = PC_INIT_STATE;
    int64_t last_stun_ms_ = -1;

private:
    UdpClient* udp_client_ = nullptr;
    std::string pc_ipaddr_str_;
    uint16_t pc_udp_port_ = 0;
    RtcDtls dtls_;
    SdpTransform offer_sdp_;
    SdpTransform answer_sdp_;

    std::shared_ptr<SRtpSession> write_srtp_;
    std::shared_ptr<SRtpSession> read_srtp_;

private:
    bool has_rtx_ = false;
    std::shared_ptr<RtcSendStream> video_send_stream_;
    std::shared_ptr<RtcSendStream> audio_send_stream_;

private:
    std::shared_ptr<RtcRecvStream> video_recv_stream_;
    std::shared_ptr<RtcRecvStream> audio_recv_stream_;

private:
    NTP_TIMESTAMP last_xr_ntp_ = {0, 0};
    int64_t last_xr_ms_ = -1;
    int64_t last_rr_ms_ = -1;
    int64_t last_send_xr_dlrr_ms_ = -1;

private:
    int64_t last_statics_ms_ = -1;

private:
    JitterBuffer jb_video_;
    JitterBuffer jb_audio_;

private:
    std::shared_ptr<PackHandleBase> video_pack_;
    std::shared_ptr<PackHandleBase> audio_pack_;
    bool find_keyframe_         = false;

private:
    MediaCallbackI* media_cb_ = nullptr;

private://for rtp extern header
    std::map<int, RTP_EXT_INFO> rtp_ext_headers_;

};

}

#endif
