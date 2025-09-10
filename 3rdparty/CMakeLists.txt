include(ExternalProject)

set(EXTRA_CFLAGS "-I${PREFIX_DIR}/include")
set(EXTRA_LDFLAGS "-L${PREFIX_DIR}/lib")
set(EXTRA_LIBS "-lm -pthread -ldl")


set(TEST_APPS OFF CACHE INTERNAL "")
set(BUILD_SHARED_LIBS ON CACHE INTERNAL "")
set(ENABLE_OPENSSL ON CACHE INTERNAL "")
add_subdirectory(libsrtp)
add_library(libsrtp ALIAS srtp2)
add_subdirectory(libuv)
