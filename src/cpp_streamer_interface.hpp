#ifndef CPP_STREAMER_INTERFACE_H
#define CPP_STREAMER_INTERFACE_H
#include "media_packet.hpp"
#include "logger.hpp"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <map>
#undef STREAMER_API
#if defined(_WIN32)
#if defined(STREAMER_EXPORT)
#define STREAMER_API _declspec(dllexport)
#else
#define STREAMER_API _declspec(dllimport)
#endif
//#define STREAMER_API
#define strcasecmp _stricmp
#else
#define STREAMER_API __attribute__((visibility("default")))
#endif

namespace cpp_streamer
{

STREAMER_API class StreamerReport
{
public:
    StreamerReport() = default;
    virtual ~StreamerReport() = default;

public:
    virtual void OnReport(const char* name, const char* type, const char* value) = 0;
};

STREAMER_API class CppStreamerInterface
{
public:
    CppStreamerInterface() = default;
    virtual ~CppStreamerInterface() = default;

public:
    virtual const char* StreamerName() = 0;
    virtual void SetLogger(Logger* logger) = 0;
    virtual int AddSinker(CppStreamerInterface* sinker) = 0;
    virtual int RemoveSinker(const char* name) = 0;
    virtual int SourceData(Media_Packet_Ptr pkt_ptr) = 0;
    virtual void StartNetwork(const char* url, void* loop_handle) = 0;
    virtual void AddOption(const char* key, const char* value) = 0;
    virtual void SetReporter(StreamerReport* reporter) = 0;

protected:
    Logger* logger_ = nullptr;
    std::string name_;
    std::map<std::string, CppStreamerInterface*> sinkers_;
    std::map<std::string, std::string> options_;
    StreamerReport* report_ = nullptr;
};


}

#endif
