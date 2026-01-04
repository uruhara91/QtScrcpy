set(QSC_PROJECT_NAME "QtScrcpyCore")

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# --- STANDARD SETUP ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt version
if (NOT QT_DESIRED_VERSION)
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
    message("   >>> Found Qt version: ${QT_VERSION_MAJOR}.${QT_VERSION_MINOR}.${QT_VERSION_PATCH}")
    set(QT_DESIRED_VERSION ${QT_VERSION_MAJOR})
endif()

find_package(Qt${QT_DESIRED_VERSION} REQUIRED COMPONENTS Widgets Network)

set(LINK_LIBS
    Qt${QT_DESIRED_VERSION}::Widgets
    Qt${QT_DESIRED_VERSION}::Network
)

# check arch
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(QSC_CPU_ARCH x64)
else()
    set(QSC_CPU_ARCH x86)
endif()

# MacOS Support
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES arm64)
    endif()
    if (CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        set(QSC_CPU_ARCH arm64)
    endif()
endif()

message(STATUS "[${PROJECT_NAME}] QSC_CPU_ARCH:${QSC_CPU_ARCH}")

# Sources
file(GLOB_RECURSE QSC_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
)

# Resources
file(GLOB QSC_RCS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.qrc")

# Add Library
add_library(${QSC_PROJECT_NAME} STATIC ${QSC_SRCS} ${QSC_RCS})

# --- INCLUDE DIRECTORIES (CRITICAL FIX) ---
# Menambahkan folder sub-modul agar header seperti 'input.h' bisa ditemukan langsung
target_include_directories(${QSC_PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/src/device/android
    ${CMAKE_CURRENT_SOURCE_DIR}/src/device/controller
    ${CMAKE_CURRENT_SOURCE_DIR}/src/device/decoder
    ${CMAKE_CURRENT_SOURCE_DIR}/src/device/server
    ${CMAKE_CURRENT_SOURCE_DIR}/src/device/demuxer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/device/recorder
    ${CMAKE_CURRENT_SOURCE_DIR}/src/devicemanage
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# --- PLATFORM SPECIFIC (CLEAN SW STACK) ---

# Windows
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/ffmpeg")
    target_include_directories(${QSC_PROJECT_NAME} PRIVATE ${FFMPEG_DIR}/include)
    target_link_libraries(${QSC_PROJECT_NAME} PRIVATE
        ${FFMPEG_DIR}/lib/${QSC_CPU_ARCH}/avcodec.lib
        ${FFMPEG_DIR}/lib/${QSC_CPU_ARCH}/avformat.lib
        ${FFMPEG_DIR}/lib/${QSC_CPU_ARCH}/avutil.lib
        ${FFMPEG_DIR}/lib/${QSC_CPU_ARCH}/swscale.lib
    )
endif()

# MacOS
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(FFMPEG_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/ffmpeg/lib/${QSC_CPU_ARCH}")
    target_include_directories(${QSC_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/ffmpeg/include
    )
    target_link_libraries(${QSC_PROJECT_NAME} PRIVATE
        ${FFMPEG_LIB_DIR}/libavcodec.a
        ${FFMPEG_LIB_DIR}/libavformat.a
        ${FFMPEG_LIB_DIR}/libavutil.a
        ${FFMPEG_LIB_DIR}/libswscale.a
        ${FFMPEG_LIB_DIR}/libavdevice.a
        ${FFMPEG_LIB_DIR}/libavfilter.a
        ${FFMPEG_LIB_DIR}/libswresample.a
        "-framework VideoToolbox"
        "-framework CoreVideo"
        "-framework CoreFoundation"
        "-framework CoreMedia"
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework Security"
        iconv
        z
        bz2
    )
    
    set(QSC_DEPLOY_PATH "${CMAKE_BINARY_DIR}/bin/${CMAKE_CFG_INTDIR}/${QC_PROJECT_NAME}.app/Contents/MacOS")
    add_custom_command(TARGET ${QSC_PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${QSC_DEPLOY_PATH}/MacOS"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/adb/mac/adb" "${QSC_DEPLOY_PATH}/MacOS"
    )
endif()

# Linux (THE OPTIMIZED PART)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    
    set(FFMPEG_STATIC_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/ffmpeg/lib")

    target_include_directories(${QSC_PROJECT_NAME} PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/ffmpeg/include
    )

    # Link libraries: Hanya yang wajib untuk SW Decode
    target_link_libraries(${QSC_PROJECT_NAME} PRIVATE
        # FFmpeg Static
        ${FFMPEG_STATIC_LIB_DIR}/libavformat.a
        ${FFMPEG_STATIC_LIB_DIR}/libavcodec.a
        ${FFMPEG_STATIC_LIB_DIR}/libavutil.a
        ${FFMPEG_STATIC_LIB_DIR}/libswscale.a
        
        # System
        pthread
        z
    )
    
    # Copy ADB & Server
    get_target_property(QSC_BIN_OUTPUT_PATH ${QSC_PROJECT_NAME} RUNTIME_OUTPUT_DIRECTORY)
    if (NOT QSC_BIN_OUTPUT_PATH)
        set(QSC_BIN_OUTPUT_PATH ${CMAKE_BINARY_DIR})
    endif()
    
    add_custom_command(TARGET ${QSC_PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/adb/linux/adb" "${QSC_BIN_OUTPUT_PATH}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/scrcpy-server" "${QSC_BIN_OUTPUT_PATH}"
    )
endif()

# Global Link (Qt)
target_link_libraries(${QSC_PROJECT_NAME} PUBLIC ${LINK_LIBS})