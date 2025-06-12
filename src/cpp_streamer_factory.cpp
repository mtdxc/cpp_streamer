#include "cpp_streamer_factory.hpp"
#include "logger.hpp"
#ifdef _WIN32
#include <Windows.h>
#define RTLD_LAZY 0
void* dlopen(const char* pathname, int mode) {
    return LoadLibrary(pathname);
}
void* dlsym(void* handle, const char* symbol) {
    return GetProcAddress((HMODULE)handle, symbol);
}
const char* dlerror() {
    int err = GetLastError();
    static char buff[20];
    sprintf(buff, "%d", err);
    return buff;
}
#else
#include <dlfcn.h>
#endif // !_WIN32

#include <stdio.h>

namespace cpp_streamer
{
using MAKE_STREAMER_FUN_PTR = void*(*)();
using DESTROY_STREAMER_FUN_PTR = void(*)(void*);

std::string CppStreamerFactory::lib_path_ = DEF_LIB_PATH;
Logger* CppStreamerFactory::s_logger_ = nullptr;
std::map<std::string, void*> CppStreamerFactory::name2handle_;

void CppStreamerFactory::SetLibPath(const char* path) {
    lib_path_ = path;
}

void* CppStreamerFactory::GetHandle(const char* streamer_name) {
    void* handle = nullptr;
    if (name2handle_.find(streamer_name) == name2handle_.end()) {
        std::string name = CppStreamerFactory::lib_path_;
        char dllName[64];
#ifdef _WIN32
        sprintf(dllName, "%s.dll", streamer_name);
#elif __linux__
        sprintf(dllName, "lib%s.so", streamer_name);
#elif __APPLE__
        sprintf(dllName, "lib%s.dylib", streamer_name);
#endif
        LogInfof(s_logger_, "try to dlopen %s", dllName);
        handle = dlopen((name + "/" + dllName).c_str(), RTLD_LAZY);
        if (handle == nullptr) {
            handle = dlopen(dllName, RTLD_LAZY);
            if (handle == nullptr) {
                LogErrorf(s_logger_, "dlopen %s error", dllName);
                return nullptr;
            }
        }
        name2handle_[streamer_name] = handle;
    } else {
        handle = name2handle_[streamer_name];
    }
    return handle;
}

CppStreamerInterface* CppStreamerFactory::MakeStreamer(const char* streamer_name) {
    void* handle = GetHandle(streamer_name);
    
    char make_func_name[80];
    snprintf(make_func_name, sizeof(make_func_name), "make_%s_streamer", streamer_name);
    LogInfof(s_logger_, "call function:%s", make_func_name);
    MAKE_STREAMER_FUN_PTR maker_fun = (MAKE_STREAMER_FUN_PTR)dlsym(handle, make_func_name);
    if (maker_fun  == NULL) {
        LogErrorf(s_logger_, "Symbol %s not found: %s", make_func_name, dlerror());
        return nullptr;
    }
    return (CppStreamerInterface*)maker_fun();
}

void CppStreamerFactory::DestroyStreamer(const char* streamer_name, CppStreamerInterface* streamer) {
    char* err_msg = nullptr;
    void* handle = GetHandle(streamer_name);
    DESTROY_STREAMER_FUN_PTR destroy_fun = (DESTROY_STREAMER_FUN_PTR)dlsym(handle, "destroy_streamer");
    if (destroy_fun == NULL) {
        LogErrorf(s_logger_, "Symbol destroy_streamer not found: %s", dlerror());
        return;
    }

    destroy_fun((void*)streamer);

    return;
}

void CppStreamerFactory::ReleaseAll() {
    for (auto& item : name2handle_) {
        item.second = nullptr;
    }
}
}
