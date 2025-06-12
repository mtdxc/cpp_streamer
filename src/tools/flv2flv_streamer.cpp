#include "cpp_streamer_interface.hpp"
#include "cpp_streamer_factory.hpp"
#include "logger.hpp"
#include "media_packet.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <getopt.h>

using namespace cpp_streamer;

static Logger* s_logger = nullptr;

class Flv2FlvStreamerMgr : public CppStreamerInterface, public StreamerReport
{
public:
    Flv2FlvStreamerMgr(const char* output_filename):filename_(output_filename)
    {
    }
    virtual ~Flv2FlvStreamerMgr()
    {
        if (demux_) {
            delete demux_;
            demux_ = nullptr;
        }
        if (muxer_) {
            delete muxer_;
            muxer_ = nullptr;
        }
    }

public:
    int MakeStreamers() {
        demux_ = CppStreamerFactory::MakeStreamer("flvdemux");
        if (!demux_) {
            LogErrorf(logger_, "make streamer flvdemux error");
            return -1;
        }
        LogInfof(logger_, "make flv demux streamer:%p, name:%s", demux_, demux_->StreamerName());
        demux_->SetLogger(logger_);
        demux_->SetReporter(this);
 
        muxer_ = CppStreamerFactory::MakeStreamer("flvmux");
        if (!muxer_) {
            LogErrorf(logger_, "make streamer flvmux error");
            return -1;
        }
        LogInfof(logger_, "make flv mux streamer:%p, name:%s", muxer_, muxer_->StreamerName());
        muxer_->SetLogger(logger_);
        muxer_->SetReporter(this);
        demux_->AddSinker(muxer_);
        muxer_->AddSinker(this);
        return 0;
    }

    int InputFlvData(uint8_t* data, size_t data_len) {
        if (!demux_) {
            LogErrorf(logger_, "flv demux streamer is not ready");
            return -1;
        }
        //LogInfof(logger_, "input data len:%u", data_len);
        Media_Packet_Ptr pkt_ptr = std::make_shared<Media_Packet>();
        pkt_ptr->AppendData(data, data_len);
        demux_->SourceData(pkt_ptr);
        return 0;
    }

public:
    virtual void OnReport(const char* name, const char* type, const char* value) override {
        LogWarnf(logger_, "report name:%s, type:%s, value:%s", name, type, value);
    }

public:
    virtual const char* StreamerName() override {
        return "flv2flv_manager";
    }
    virtual void SetLogger(Logger* logger) override {
        logger_ = logger;
    }
    virtual int AddSinker(CppStreamerInterface* sinker) override {
        return 0;
    }
    virtual int RemoveSinker(const char* name) override {
        return 0;
    }
    virtual int SourceData(Media_Packet_Ptr pkt_ptr) override {
        FILE* file_p = fopen(filename_.c_str(), "ab+");
        if (file_p) {
            fwrite(pkt_ptr->Data(), 1, pkt_ptr->Size(), file_p);
            fclose(file_p);
        }
        return 0;
    }
    virtual void StartNetwork(const char* url, void* loop_handle) override {
        return;
    }
    virtual void AddOption(const char* key, const char* value) override {
        return;
    }
    virtual void SetReporter(StreamerReport* reporter) override {

    }

private:
    Logger* logger_ = nullptr;
    std::string filename_;
    CppStreamerInterface* demux_ = nullptr;
    CppStreamerInterface* muxer_ = nullptr;
};

int main(int argc, char** argv) {
    char input_flv_name[128];
    char output_flv_name[128];
    char log_file[128];

    bool input_flv_name_ready = false;
    bool output_flv_name_ready = false;
    bool log_file_ready = false;

    int opt = 0;
    while ((opt = getopt(argc, argv, "i:o:l:h")) != -1) {
        switch (opt) {
            case 'i': strncpy(input_flv_name, optarg, sizeof(input_flv_name)); input_flv_name_ready = true; break;
            case 'o': strncpy(output_flv_name, optarg, sizeof(output_flv_name)); output_flv_name_ready = true; break;
            case 'l': strncpy(log_file, optarg, sizeof(log_file)); log_file_ready = true; break;
            case 'h':
            default: 
            {
                printf("Usage: %s [-i flv file name]\n\
    [-o flv file name]\n\
    [-l log file name]\n",
                    argv[0]); 
                return -1;
            }
        }
    }

    if (!input_flv_name_ready || !output_flv_name_ready) {
        std::cout << "please input/output flv name\r\n";
        return -1;
    }

    s_logger = new Logger();
    if (log_file_ready) {
        s_logger->SetFilename(log_file);
    }

    CppStreamerFactory::SetLogger(s_logger);
    CppStreamerFactory::SetLibPath("./output/lib");

    LogInfof(s_logger, "flv2flv streamer manager is starting, input filename:%s, output filename:%s",
            input_flv_name, output_flv_name);

    auto streamer_mgr_ptr = std::make_shared<Flv2FlvStreamerMgr>(output_flv_name);
    streamer_mgr_ptr->SetLogger(s_logger);
    if (streamer_mgr_ptr->MakeStreamers() < 0) {
        LogErrorf(s_logger, "call GenFlvDemuxStreamer error");
        return -1;
    }
    FILE* file_p = fopen(input_flv_name, "rb");
    if (!file_p) {
        LogErrorf(s_logger, "open flv file error:%s", input_flv_name);
        return -1;
    }
    uint8_t read_data[2048];
    size_t read_n = 0;
    do {
        read_n = fread(read_data, 1, sizeof(read_data), file_p);
        if (read_n > 0) {
            streamer_mgr_ptr->InputFlvData(read_data, read_n);
        }
    } while (read_n > 0);
    fclose(file_p);

    //std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    LogInfof(s_logger, "flv2flv done");

    streamer_mgr_ptr = nullptr;
    CppStreamerFactory::ReleaseAll();
    
    getchar();
    delete s_logger;

    return 0;
}
