#ifndef CPP_STREAMER_FACTORY
#define CPP_STREAMER_FACTORY
#include "media_packet.hpp"
#include "cpp_streamer_interface.hpp"
#include "logger.hpp"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <map>

namespace cpp_streamer
{

#define DEF_LIB_PATH "/usr/local/lib"

class CppStreamerFactory
{
public:
    static void SetLibPath(const char* path);
    static void SetLogger(Logger* logger) {
        s_logger_ = logger;
    }
    static CppStreamerInterface* MakeStreamer(const char* streamer_name);
    static void DestroyStreamer(const char* streamer_name, CppStreamerInterface* streamer);
    static void ReleaseAll();

private:
    static void* GetHandle(const char* streamer_name);

private:
    static std::string lib_path_;
    static Logger* s_logger_;
    static std::map<std::string, void*> name2handle_;
};

}
#endif
