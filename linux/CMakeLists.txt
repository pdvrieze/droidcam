cmake_minimum_required(VERSION 3.15)

project(droidcam)
set(COMMON_SOURCE src/connection.cpp src/decoder.cpp src/util.cpp src/util.h src/jpegpp.cpp src/jpegpp.h)

#set(CMAKE_CXX_COMPILER clang)
set(CMAKE_CXX_FLAGS_DEBUG  "${CMAKE_C_FLAGS_DEBUG} -g -march=native -mtune=native")
#set(CMAKE_CXX_FLAGS_DEBUG  "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address -fsanitize-recover -g -march=native -mtune=native")
set(CMAKE_CXX_FLAGS_RELEASE "-march=native -mtune=native -O2 -Wall -march=native -mtune=native")
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

include(FindPkgConfig)
include(FindCURL)

#find_package(GTK2 2.6 REQUIRED gtk)
find_path(SWSCALE_INCLUDE_DIRS libswscale/swscale.h)
find_library(SWSCALE_LIBRARY swscale)
find_library(M m)

pkg_check_modules(JPEGTURBO REQUIRED libturbojpeg)
pkg_check_modules(JPEG REQUIRED libjpeg)
pkg_check_modules(GTK2 REQUIRED gtk+-2.0)
pkg_check_modules(GTHREAD2 REQUIRED gthread-2.0)


add_executable(droidcam ${COMMON_SOURCE} src/droidcam.cpp src/context.h src/ipcam.h src/ipcam.cpp src/droidcam.h src/droidcamBase.cpp src/droidcamBase.h)
add_executable(droidcam-cli ${COMMON_SOURCE} src/droidcam-cli.cpp)

include_directories(${SWSCALE_INCLUDE_DIRS})
include_directories(${JPEG_INCLUDE_DIRS})

target_include_directories(droidcam PUBLIC "${GTK2_INCLUDE_DIRS}")
target_include_directories(droidcam PUBLIC "${CURL_INCLUDE_DIRS}")
#target_include_directories(droidcam PUBLIC "${GTK_INCLUDE_DIRS}")
target_link_libraries(droidcam ${GTK2_LDFLAGS})
target_link_libraries(droidcam m swscale ${JPEG_LDFLAGS} ${JPEGTURBO_LDFLAGS} ${GTHREAD2_LDFLAGS} ${CURL_LIBRARIES})
#target_link_libraries(droidcam m swscale libturbojpeg.a ${GTHREAD2_LDFLAGS})
target_link_libraries(droidcam-cli m swscale ${JPEG_LDFLAGS} ${JPEGTURBO_LDFLAGS} ${GTHREAD2_LDFLAGS})
