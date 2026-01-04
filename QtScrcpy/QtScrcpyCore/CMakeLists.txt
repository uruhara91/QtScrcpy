set(QSC_PROJECT_NAME "QtScrcpyCore")

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
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

# MacOS
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # mac default arch arm64
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES arm64)
    endif()

    if (CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        set(QSC_CPU_ARCH arm64)
    endif()
endif()

message(STATUS "[${PROJECT_NAME}] QSC_CPU_ARCH:${QSC_CPU_ARCH}")

# Sources
# (Otomatis mengambil semua file source, aman)
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

# Include Directories
target_include_directories(${QSC_PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common
)

# --- PLATFORM SPECIFIC DEPENDENCIES (CLEANED UP) ---

# Windows
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/ffmpeg")
    target_include_directories(${QSC_PROJECT_NAME} PRIVATE
        ${FFMPEG_DIR}/include
    )
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
    # Copy ADB for Mac logic preserved
    set(QSC_DEPLOY_PATH "${CMAKE_BINARY_DIR}/bin/${CMAKE_CFG_INTDIR}/${QC_PROJECT_NAME}.app/Contents/MacOS")
    add_custom_command(TARGET ${QSC_PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${QSC_DEPLOY_PATH}/MacOS"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/adb/mac/adb" "${QSC_DEPLOY_PATH}/MacOS"
    )
endif()

# --- LINUX (OPTIMIZED SW ONLY) ---
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # HAPUS: pkg_check_modules LIBVA, LIBDRM, EGL (Tidak perlu lagi!)
    
    set(FFMPEG_STATIC_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/ffmpeg/lib")

    # Include path (Cuma FFmpeg)
    target_include_directories(${QSC_PROJECT_NAME} PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/ffmpeg/include
    )

    # Link libraries (Clean SW Stack)
    target_link_libraries(${QSC_PROJECT_NAME} PRIVATE
        # FFmpeg Static Libs
        ${FFMPEG_STATIC_LIB_DIR}/libavformat.a
        ${FFMPEG_STATIC_LIB_DIR}/libavcodec.a
        ${FFMPEG_STATIC_LIB_DIR}/libavutil.a
        ${FFMPEG_STATIC_LIB_DIR}/libswscale.a # Penting untuk SW Convert/Resize
        
        # System Libs
        pthread # Wajib untuk Threading Decoder
        dl
        z
    )
    
    # Copy ADB Logic
    get_target_property(QSC_BIN_OUTPUT_PATH ${QSC_PROJECT_NAME} RUNTIME_OUTPUT_DIRECTORY)
    # Fallback if property not set
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