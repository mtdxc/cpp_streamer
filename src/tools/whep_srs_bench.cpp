#include "logger.hpp"
#include "cpp_streamer_factory.hpp"
#include "timer.hpp"

#include <iostream>
#include <uv.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string>
#include <sstream>
#include <getopt.h>
#include <chrono>
#include <thread>
#include <memory>

using namespace cpp_streamer;

static Logger* s_logger = nullptr;
static size_t WHEPS_INTERVAL = 10;

class WhepBench: public StreamerReport, public TimerInterface, public CppStreamerInterface
{
public:
    WhepBench(uv_loop_t* loop, const std::string& src_url, 
            int bench_count):TimerInterface(loop, 500)
                            , bench_count_(bench_count)
                            , src_url_(src_url)
    {
        loop_ = loop;
    }
    virtual ~WhepBench()
    {
        whep_ready_ = false;
        StopTimer();
    }

    int MakeStreamers() {
        for (size_t i = 0; i < bench_count_; i++) {
            CppStreamerInterface* mediasoup_puller = CppStreamerFactory::MakeStreamer("whep");
            mediasoup_puller->SetLogger(logger_);
            mediasoup_puller->SetReporter(this);
            mediasoup_puller->AddSinker(this);

            srs_whep_vec.push_back(mediasoup_puller);
        }

        return 0;
    }

    void Start() {
        if (start_) {
            return;
        }
        start_ = true;
        StartTimer();
    }

    void Stop() {
        if (!start_) {
            return;
        }
        start_ = false;
        Clean();
        LogInfof(logger_, "job is done.");
        exit(0);
    }
public:
    virtual void OnReport(const std::string& name,
            const std::string& type,
            const std::string& value) override {
        LogWarnf(logger_, "report name:%s, type:%s, value:%s",
                name.c_str(), type.c_str(), value.c_str());
        if (value == "ready") {
            int index = GetWhepIndex(name);
            if (index < 0) {
                LogErrorf(logger_, "fail to get whep streamer by name:%s", name.c_str());
            } else {
                LogWarnf(logger_, "whep streamer %d is ready, name:%s", index, name.c_str());
            }
        }
    }

public:
    virtual std::string StreamerName() override {
        return "whepbench";
    }
    virtual void SetLogger(Logger* logger) override {
        logger_ = logger;
    }
    virtual int AddSinker(CppStreamerInterface* sinker) override {
        return 0;
    }

    virtual int RemoveSinker(const std::string& name) override {
        return 0;
    }
    virtual int SourceData(Media_Packet_Ptr pkt_ptr) override {
        return 0;
    }
    virtual void StartNetwork(const std::string& url, void* loop_handle) override {

    }
    virtual void AddOption(const std::string& key, const std::string& value) override {

    }
    virtual void SetReporter(StreamerReport* reporter) override {

    }
//TimerInterface
protected:
    virtual void OnTimer() override {
        StartWheps();
    }

private:
    void StartWheps() {
        if (post_done_) {
            return;
        }

        LogWarnf(logger_, "StartWheps  index:%lu", whep_index_);
        size_t i = 0;
        for (i = whep_index_; i < whep_index_ + WHEPS_INTERVAL; i++) {
            if (i >= bench_count_) {
                break;
            }
            std::string url = GetUrl(i);
            LogWarnf(logger_, "start network url:%s", url.c_str());
            try {
                srs_whep_vec[i]->StartNetwork(url, loop_);
            } catch(CppStreamException& e) {
                LogErrorf(logger_, "mediasoup pull start network exception:%s", e.what());
            }
        }
        whep_index_ = i;
        if (whep_index_ >= bench_count_) {
            post_done_ = true;
        }
    }

    std::string GetUrl(size_t index) {
        std::string url = src_url_;
        url += "&sessionId=1000";
        url += std::to_string(index);
        return url;
    }
    int GetWhepIndex(const std::string& name) {
        int index = -1;
        for (CppStreamerInterface* whep : srs_whep_vec) {
            index++;
            if (!whep) {
                continue;
            }
            if (whep->StreamerName() == name) {
                return index;
            }
        }
        return -1;
    }
    void Clean() {
        for (size_t i = 0; i < bench_count_; i++) {
            CppStreamerInterface* srs_whep = srs_whep_vec[i];
            if (srs_whep) {
                delete srs_whep;
                srs_whep_vec[i] = nullptr;
            }
        }
        uv_loop_close(loop_);
    }

private:
    size_t bench_count_ = 0;
    std::string src_url_;
    uv_loop_t* loop_ = nullptr;
    bool start_ = false;
    bool whep_ready_ = false;

    bool post_done_ = false;
    size_t whep_index_ = 0;

private:
    Logger* logger_ = nullptr;
    std::vector<CppStreamerInterface*> srs_whep_vec;
};

/*
 ./whep_srs_bench -i "http://10.0.8.5:1985/rtc/v1/whip-play/?app=live&stream=1000" -n 100 -l output.log
 */
int main(int argc, char** argv) {
    char input_url_name[516];
    char log_file[516];
    int bench_count = -1;

    int opt = 0;
    bool input_url_name_ready = false;
    bool log_file_ready = false;

    while ((opt = getopt(argc, argv, "i:n:l:h")) != -1) {
        switch (opt) {
            /*eg: http://10.0.24.12:1985/rtc/v1/whip-play/?app=live&stream=1000*/
            case 'i': strncpy(input_url_name, optarg, sizeof(input_url_name)); input_url_name_ready = true; break;
            case 'n':
            {
                char count_sz[80];
                strncpy(count_sz, optarg, sizeof(count_sz));
                bench_count = atoi(count_sz);
                break;
            }
            case 'l': strncpy(log_file, optarg, sizeof(log_file)); log_file_ready = true; break;
            case 'h':
            default: 
            {
                printf("Usage: %s [-i input whep url]\n\
    [-n bench count]\n\
    [-l log file name]\n",
                    argv[0]); 
                return -1;
            }
        }
    }

    if (!input_url_name_ready) {
        std::cout << "please output whep url\r\n";
        return -1;
    }
    if (bench_count <= 0) {
        std::cout << "bench count is error.\r\n";
        return -1;
    }

    s_logger = new Logger();
    if (log_file_ready) {
        s_logger->SetFilename(std::string(log_file));
    }

    CppStreamerFactory::SetLogger(s_logger);
    CppStreamerFactory::SetLibPath("./output/lib");

    LogInfof(s_logger, "whep bench streamer is starting, input whep:%s, bench_count:%d",
            input_url_name, bench_count);
    uv_loop_t* loop = uv_default_loop();

    std::shared_ptr<WhepBench> mgr_ptr = std::make_shared<WhepBench>(loop, input_url_name, bench_count);

    mgr_ptr->SetLogger(s_logger);

    if (mgr_ptr->MakeStreamers() < 0) {
        LogErrorf(s_logger, "make whep bench streamers error");
        return -1;
    }
    LogInfof(s_logger, "whep bench is starting......");
    mgr_ptr->Start();
    while (true) {
        uv_run(loop, UV_RUN_DEFAULT);
    }
 
    return 0;
}
