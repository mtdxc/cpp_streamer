#ifndef FLV_MUX_HPP
#define FLV_MUX_HPP
#include "data_buffer.hpp"
#include "media_packet.hpp"
#include "cpp_streamer_interface.hpp"
#include "logger.hpp"

#include <map>

extern "C" {
STREAMER_API void* make_flvmux_streamer();
STREAMER_API void destroy_flvmux_streamer(void* streamer);
}

namespace cpp_streamer
{
class FlvMuxer : public CppStreamerInterface
{
public:
    FlvMuxer();
    virtual ~FlvMuxer();

public:
    virtual const char* StreamerName() override;
    virtual void SetLogger(Logger* logger) override {
        logger_ = logger;
    }
    virtual int AddSinker(CppStreamerInterface* sinker) override;
    virtual int RemoveSinker(const char* name) override;
    virtual int SourceData(Media_Packet_Ptr pkt_ptr) override;
    virtual void StartNetwork(const char* url, void* loop_handle) override;
    virtual void AddOption(const char* key, const char* value) override;
    virtual void SetReporter(StreamerReport* reporter) override;

public:
    int InputPacket(Media_Packet_Ptr pkt_ptr);

private:
    int MuxFlvHeader(Media_Packet_Ptr pkt_ptr);
    void OutputPacket(Media_Packet_Ptr pkt_ptr);
    void Report(const std::string& type, const std::string& value);

private:
    bool has_video_ = true;
    bool has_audio_ = true;

private:
    uint8_t sps_[128];
    uint8_t pps_[128];
    int sps_len_ = -1;
    int pps_len_ = -1;
    bool first_video_ = false;

private:
    bool header_ready_ = false;

private:
    static std::map<std::string, std::string> def_options_;
};

}
#endif
