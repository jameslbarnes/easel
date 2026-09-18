cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
    project(Easel VERSION 0.2.0 LANGUAGES C CXX OBJCXX)
else()
    project(Easel VERSION 0.2.0 LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch all dependencies
include(cmake/FetchDependencies.cmake)

# FFmpeg
set(FFMPEG_FOUND FALSE)
if(WIN32)
    set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/external/ffmpeg-master-latest-win64-gpl-shared")
    if(EXISTS "${FFMPEG_DIR}")
        set(FFMPEG_FOUND TRUE)
        message(STATUS "FFmpeg found at ${FFMPEG_DIR}")
    endif()
elseif(APPLE)
    set(FFMPEG_DIR "/opt/homebrew/opt/ffmpeg")
    if(NOT EXISTS "${FFMPEG_DIR}")
        set(FFMPEG_DIR "/usr/local/opt/ffmpeg")
    endif()
    if(EXISTS "${FFMPEG_DIR}")
        set(FFMPEG_FOUND TRUE)
        message(STATUS "FFmpeg found at ${FFMPEG_DIR}")
    endif()
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
            libavformat libavcodec libavutil libswscale libswresample)
        if(FFMPEG_FOUND)
            message(STATUS "FFmpeg found via pkg-config")
        endif()
    endif()
endif()
if(NOT FFMPEG_FOUND)
    message(WARNING "FFmpeg not found - video support disabled")
endif()

# OpenCV
if(WIN32)
    set(OPENCV_DIR "${CMAKE_SOURCE_DIR}/external/opencv")
    if(EXISTS "${OPENCV_DIR}")
        set(OpenCV_DIR "${OPENCV_DIR}/x64/vc16/lib")
        find_package(OpenCV QUIET COMPONENTS core imgproc calib3d videoio)
        if(OpenCV_FOUND)
            set(HAS_OPENCV TRUE)
            message(STATUS "OpenCV ${OpenCV_VERSION} found at ${OPENCV_DIR}")
        else()
            set(HAS_OPENCV FALSE)
            message(WARNING "OpenCV found but find_package failed - scanning disabled")
        endif()
    else()
        set(HAS_OPENCV FALSE)
        message(STATUS "OpenCV not found - scanning disabled")
    endif()
else()
    find_package(OpenCV QUIET COMPONENTS core imgproc calib3d videoio)
    if(OpenCV_FOUND)
        set(HAS_OPENCV TRUE)
        message(STATUS "OpenCV ${OpenCV_VERSION} found")
    else()
        set(HAS_OPENCV FALSE)
        message(STATUS "OpenCV not found - scanning disabled")
    endif()
endif()

# Whisper.cpp disabled (CUDA compilation too slow)
set(HAS_WHISPER FALSE)
message(STATUS "whisper.cpp disabled")

# Spout2 (compiled from source)
set(SPOUT_DIR "${CMAKE_SOURCE_DIR}/external/Spout2/SPOUTSDK/SpoutGL")
if(EXISTS "${SPOUT_DIR}/Spout.h")
    set(HAS_SPOUT TRUE)
    file(GLOB SPOUT_SOURCES "${SPOUT_DIR}/*.cpp")
    message(STATUS "Spout2 SDK found at ${SPOUT_DIR}")
else()
    set(HAS_SPOUT FALSE)
    message(STATUS "Spout2 SDK not found - Spout support disabled")
endif()

# NDI (headers only - runtime loaded dynamically)
set(NDI_DIR "${CMAKE_SOURCE_DIR}/external/ndi")
if(EXISTS "${NDI_DIR}/include/Processing.NDI.Lib.h")
    set(HAS_NDI TRUE)
    message(STATUS "NDI SDK headers found at ${NDI_DIR}")
else()
    set(HAS_NDI FALSE)
    message(STATUS "NDI SDK headers not found - NDI support disabled")
endif()

# Collect source files
file(GLOB_RECURSE SOURCES
    src/*.cpp
    src/*.h
)

# On macOS, also collect Objective-C++ sources
if(APPLE)
    file(GLOB_RECURSE OBJCXX_SOURCES src/*.mm)
    list(APPEND SOURCES ${OBJCXX_SOURCES})
endif()

# Exclude test harness and VideoSource as needed
list(FILTER SOURCES EXCLUDE REGEX ".*test_shaders.*")
if(NOT FFMPEG_FOUND)
    list(FILTER SOURCES EXCLUDE REGEX ".*VideoSource.*")
endif()
if(NOT HAS_OPENCV)
    list(FILTER SOURCES EXCLUDE REGEX ".*scanning.*")
endif()
if(NOT HAS_WHISPER)
    list(FILTER SOURCES EXCLUDE REGEX ".*WhisperSpeech.*")
endif()
list(FILTER SOURCES EXCLUDE REGEX ".*SpeechBridge.*")
if(NOT HAS_NDI)
    list(FILTER SOURCES EXCLUDE REGEX ".*NDI.*")
endif()

# WHEP (WebRTC-HTTP Egress Protocol) - requires libdatachannel + FFmpeg + OpenSSL.
# Off by default: libdatachannel/libsrtp need OpenSSL dev libs, which aren't
# present on this machine. Configure with -DEASEL_ENABLE_WHEP=ON (after
# installing OpenSSL) to build it.
option(EASEL_ENABLE_WHEP "Build WHEP/WebRTC source (needs libdatachannel + OpenSSL)" OFF)
if(FFMPEG_FOUND AND EASEL_ENABLE_WHEP)
    FetchContent_MakeAvailable(libdatachannel)
    set(HAS_WHEP TRUE)
    message(STATUS "WHEP source enabled (libdatachannel + FFmpeg)")
else()
    set(HAS_WHEP FALSE)
    list(FILTER SOURCES EXCLUDE REGEX ".*WHEP.*")
    message(STATUS "WHEP source disabled (set -DEASEL_ENABLE_WHEP=ON + OpenSSL to enable)")
endif()

# Platform-specific source exclusions
if(APPLE)
    list(FILTER SOURCES EXCLUDE REGEX ".*CaptureSource\\.cpp$")
    list(FILTER SOURCES EXCLUDE REGEX ".*CaptureSource\\.h$")
    list(FILTER SOURCES EXCLUDE REGEX ".*WindowCaptureSource\\.cpp$")
    list(FILTER SOURCES EXCLUDE REGEX ".*WindowCaptureSource\\.h$")
    list(FILTER SOURCES EXCLUDE REGEX ".*WGCCapture\\.cpp$")
    list(FILTER SOURCES EXCLUDE REGEX ".*WGCCapture\\.h$")
elseif(WIN32)
    list(FILTER SOURCES EXCLUDE REGEX ".*_mac\\.mm$")
    list(FILTER SOURCES EXCLUDE REGEX ".*_mac\\.h$")
else()
    list(FILTER SOURCES EXCLUDE REGEX ".*_mac\\.mm$")
    list(FILTER SOURCES EXCLUDE REGEX ".*_mac\\.h$")
    list(FILTER SOURCES EXCLUDE REGEX ".*CaptureSource\\.cpp$")
    list(FILTER SOURCES EXCLUDE REGEX ".*CaptureSource\\.h$")
    list(FILTER SOURCES EXCLUDE REGEX ".*WindowCaptureSource\\.cpp$")
    list(FILTER SOURCES EXCLUDE REGEX ".*WindowCaptureSource\\.h$")
    list(FILTER SOURCES EXCLUDE REGEX ".*WGCCapture\\.cpp$")
    list(FILTER SOURCES EXCLUDE REGEX ".*WGCCapture\\.h$")
endif()

# kiss_fft (public domain FFT library for AudioAnalyzer)
set(KISS_FFT_SRC "${CMAKE_SOURCE_DIR}/external/kiss_fft/kiss_fft.c")

# Main executable
if(APPLE)
    # Build as a proper .app bundle so we get a dock icon, Cmd+Tab name,
    # and menu bar identity. Resources/ holds icon + shaders + default
    # preset; main_mac.mm chdirs to it on launch so relative loads work.
    set(EASEL_ICON "${CMAKE_SOURCE_DIR}/resources/icon.icns")
    set_source_files_properties("${EASEL_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    file(GLOB EASEL_SHADERS "${CMAKE_SOURCE_DIR}/shaders/*")
    foreach(s ${EASEL_SHADERS})
        set_source_files_properties("${s}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources/shaders")
    endforeach()
    # Particle subsystem shaders (nested under shaders/particles/).
    file(GLOB EASEL_PARTICLE_SHADERS "${CMAKE_SOURCE_DIR}/shaders/particles/*")
    foreach(p ${EASEL_PARTICLE_SHADERS})
        set_source_files_properties("${p}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources/shaders/particles")
    endforeach()
    # gl-transitions.com shaders bundled as-is. App scans
    # assets/transitions/gl on startup — main_mac.mm chdirs to Resources/.
    file(GLOB EASEL_GL_TRANSITIONS "${CMAKE_SOURCE_DIR}/assets/transitions/gl/*.glsl")
    foreach(g ${EASEL_GL_TRANSITIONS})
        set_source_files_properties("${g}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources/assets/transitions/gl")
    endforeach()
    # Inspector tab icons (Properties / Mapping / Audio / MIDI).
    file(GLOB EASEL_TAB_ICONS "${CMAKE_SOURCE_DIR}/assets/icons/*.png")
    foreach(i ${EASEL_TAB_ICONS})
        set_source_files_properties("${i}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources/assets/icons")
    endforeach()
    set(EASEL_DEFAULT_PRESET "${CMAKE_SOURCE_DIR}/default.easel")
    set_source_files_properties("${EASEL_DEFAULT_PRESET}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")

    # 3D model assets (e.g. the F-117 used by the "Moving Company" source).
    # Bundled read-only into Resources/assets/models — the source loads them
    # relative to the chdir'd Resources dir (main_mac.mm).
    file(GLOB EASEL_MODELS "${CMAKE_SOURCE_DIR}/assets/models/*.glb")
    foreach(m ${EASEL_MODELS})
        set_source_files_properties("${m}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources/assets/models")
    endforeach()

    add_executable(${PROJECT_NAME} MACOSX_BUNDLE
        ${SOURCES} ${KISS_FFT_SRC}
        "${EASEL_ICON}" ${EASEL_SHADERS} ${EASEL_PARTICLE_SHADERS} ${EASEL_GL_TRANSITIONS} ${EASEL_TAB_ICONS} ${EASEL_MODELS} "${EASEL_DEFAULT_PRESET}")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Easel"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.etherealmachine.easel"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "icon.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/MacOSXBundleInfo.plist.in")
elseif(WIN32)
    file(WRITE "${CMAKE_BINARY_DIR}/easel_icon.rc" "1 ICON \"${CMAKE_SOURCE_DIR}/resources/icon.ico\"\n")
    if(HAS_SPOUT)
        add_library(spout_gl STATIC ${SPOUT_SOURCES})
        target_include_directories(spout_gl PUBLIC "${SPOUT_DIR}")
        # Spout needs full Windows headers (no LEAN_AND_MEAN) for multimedia timer APIs
        target_compile_definitions(spout_gl PRIVATE NOMINMAX)
        target_link_libraries(spout_gl PRIVATE d3d11 d3d9 dxguid shlwapi winmm)
    endif()
    add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${KISS_FFT_SRC} "${CMAKE_BINARY_DIR}/easel_icon.rc")
else()
    add_executable(${PROJECT_NAME} ${SOURCES} ${KISS_FFT_SRC})
endif()

find_package(OpenGL REQUIRED)

# Shader test harness
add_executable(test_shaders
    src/test_shaders.cpp
    src/sources/ShaderSource.cpp
    src/render/ShaderProgram.cpp
    src/render/Framebuffer.cpp
    src/render/Mesh.cpp
    src/render/Texture.cpp
    src/render/FontAtlas.cpp
    src/render/GlyphBuffer.cpp
    src/app/MIDIManager.cpp
)
if(APPLE)
    target_sources(test_shaders PRIVATE src/app/MIDIManager_mac.mm)
endif()
if(WIN32)
    target_compile_definitions(test_shaders PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_include_directories(test_shaders PRIVATE ${CMAKE_SOURCE_DIR}/src ${stb_SOURCE_DIR})
target_link_libraries(test_shaders PRIVATE glfw glad glm::glm nlohmann_json::nlohmann_json OpenGL::GL)
if(APPLE)
    target_link_libraries(test_shaders PRIVATE
        "-framework CoreMIDI"
        "-framework CoreFoundation"
    )
endif()

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${stb_SOURCE_DIR}
    ${tinyobjloader_SOURCE_DIR}
    ${tinygltf_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/external/kiss_fft
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    glfw glad glm::glm imgui nlohmann_json::nlohmann_json OpenGL::GL
)
# NOTE: zlib ('z') was dropped from the link list — gzipCompress() is stubbed
# out in this build (no zlib dev libs on Windows). Re-add 'z' (or zlibstatic)
# here if you reinstate gzip compression. See the note in Application.cpp.

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dwmapi d3d11 dxgi comdlg32 ws2_32 runtimeobject iphlpapi
    )
elseif(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreVideo"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreMIDI"
        "-framework CoreFoundation"
        "-framework SystemConfiguration"
        "-framework ScreenCaptureKit"
        "-framework CoreGraphics"
        "-framework CoreMedia"
        "-framework IOSurface"
        "-framework VideoToolbox"
        "-framework Speech"           # SFSpeechRecognizer (push-to-talk)
        "-framework AVFoundation"     # AVAudioEngine for mic capture
        "-framework Metal"            # Phase M.0+ Metal abstraction layer
        "-framework Vision"           # VisionTracker — hand/face/pose ML
    )
endif()

# FFmpeg linking
if(FFMPEG_FOUND)
    if(TARGET PkgConfig::FFMPEG)
        target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::FFMPEG)
    else()
        target_include_directories(${PROJECT_NAME} PRIVATE "${FFMPEG_DIR}/include")
        target_link_directories(${PROJECT_NAME} PRIVATE "${FFMPEG_DIR}/lib")
        target_link_libraries(${PROJECT_NAME} PRIVATE avformat avcodec avutil swscale swresample)
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_FFMPEG=1)

    if(WIN32)
        file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FFMPEG_DLLS} $<TARGET_FILE_DIR:${PROJECT_NAME}>
        )
    endif()
endif()

# OpenCV linking
if(HAS_OPENCV)
    if(WIN32)
        target_include_directories(${PROJECT_NAME} PRIVATE "${OPENCV_DIR}/include")
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenCV_LIBS})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_OPENCV=1)

    if(WIN32)
        file(GLOB OPENCV_DLLS "${OPENCV_DIR}/x64/vc16/bin/*.dll")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${OPENCV_DLLS} $<TARGET_FILE_DIR:${PROJECT_NAME}>
        )
    endif()
endif()

if(HAS_WHISPER)
    target_link_libraries(${PROJECT_NAME} PRIVATE whisper)
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/external" "${whisper_SOURCE_DIR}/include")
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_WHISPER=1)
endif()

if(HAS_NDI)
    target_include_directories(${PROJECT_NAME} PRIVATE "${NDI_DIR}/include")
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_NDI=1)

    # NDI is dlopen-loaded at runtime. NDIRuntime.cpp searches next to the
    # executable; on macOS that's Easel.app/Contents/MacOS, so copy the
    # dylib in there post-build. Source is build/libndi.dylib (downloaded
    # via the NDI runtime install steps in CLAUDE.md).
    if(APPLE AND EXISTS "${CMAKE_BINARY_DIR}/libndi.dylib")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/libndi.dylib"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/libndi.dylib")
    endif()
endif()

if(HAS_WHEP)
    target_link_libraries(${PROJECT_NAME} PRIVATE LibDataChannel::LibDataChannel)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_WHEP=1)
    if(NOT WIN32)
        find_package(CURL REQUIRED)
        target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl)
    endif()
    if(APPLE)
        target_link_libraries(${PROJECT_NAME} PRIVATE "-framework WebKit")
    endif()
endif()

# Spout2 (static lib, uses DirectX shared textures, Windows-only)
if(WIN32 AND HAS_SPOUT)
    target_include_directories(${PROJECT_NAME} PRIVATE "${SPOUT_DIR}")
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_SPOUT=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE spout_gl)
endif()

# Test suites
if(HAS_OPENCV)
    add_executable(test_scanning
        tests/test_scanning.cpp src/scanning/GrayCodePattern.cpp src/scanning/SceneScanner.cpp
        src/scanning/WebcamSource.cpp src/render/Texture.cpp src/render/Framebuffer.cpp
        src/render/Mesh.cpp src/render/ShaderProgram.cpp
    )
    target_compile_definitions(test_scanning PRIVATE HAS_OPENCV=1)
    if(WIN32)
        target_compile_definitions(test_scanning PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_include_directories(test_scanning PRIVATE "${OPENCV_DIR}/include")
    endif()
    target_include_directories(test_scanning PRIVATE ${CMAKE_SOURCE_DIR}/src ${stb_SOURCE_DIR})
    target_link_libraries(test_scanning PRIVATE
        Catch2::Catch2WithMain ${OpenCV_LIBS} glfw glad glm::glm nlohmann_json::nlohmann_json OpenGL::GL
    )
    if(WIN32)
        add_custom_command(TARGET test_scanning POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENCV_DLLS} $<TARGET_FILE_DIR:test_scanning>
        )
    endif()
    include(CTest)
    include(Catch)
    catch_discover_tests(test_scanning)
endif()

set(TEST_AUDIO_SOURCES
    tests/test_audio_analyzer.cpp src/app/AudioAnalyzer.cpp
    src/render/Texture.cpp src/render/Framebuffer.cpp src/render/Mesh.cpp src/render/ShaderProgram.cpp
    external/kiss_fft/kiss_fft.c
)
if(APPLE)
    list(APPEND TEST_AUDIO_SOURCES src/app/AudioAnalyzer_mac.mm)
endif()
add_executable(test_audio_analyzer ${TEST_AUDIO_SOURCES})
if(WIN32)
    target_compile_definitions(test_audio_analyzer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_include_directories(test_audio_analyzer PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/external/kiss_fft ${stb_SOURCE_DIR}
)
target_link_libraries(test_audio_analyzer PRIVATE
    Catch2::Catch2WithMain glfw glad glm::glm nlohmann_json::nlohmann_json OpenGL::GL
)
if(APPLE)
    target_link_libraries(test_audio_analyzer PRIVATE
        "-framework ScreenCaptureKit" "-framework CoreMedia" "-framework CoreGraphics"
        "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation"
        "-framework AVFoundation"     # AVCaptureDevice mic discovery in AudioAnalyzer_mac
    )
endif()

# DataBus header-only smoke test — verifies numeric API + thread safety.
add_executable(test_databus tests/test_databus.cpp)
target_include_directories(test_databus PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_databus PRIVATE Catch2::Catch2WithMain)

# Caption contract parser (Cue camelCase + Etherea snake_case transcript shapes).
add_executable(test_caption_segment tests/test_caption_segment.cpp src/speech/CaptionSegment.cpp)
target_include_directories(test_caption_segment PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_caption_segment PRIVATE Catch2::Catch2WithMain nlohmann_json::nlohmann_json)

# SCTEXT/1 glyph-buffer encoder (pure, no GL): text -> textBuf_<n> texels + legacy shim codes.
add_executable(test_glyph_buffer tests/test_glyph_buffer.cpp src/render/GlyphBuffer.cpp)
target_include_directories(test_glyph_buffer PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_glyph_buffer PRIVATE Catch2::Catch2WithMain)

# Phase M.0 RenderBackend abstraction smoke test — disabled until RenderBackend_GL.cpp is implemented.
# add_executable(test_render_backend tests/test_render_backend.cpp src/render/RenderBackend_GL.cpp)

if(HAS_WHEP)
    set(TEST_WHEP_SOURCES
        tests/test_whep_source.cpp
        src/sources/WHEPSource.cpp
        src/render/Texture.cpp
    )
    if(APPLE)
        list(APPEND TEST_WHEP_SOURCES src/sources/WHEPSource_mac.mm)
    endif()
    add_executable(test_whep_source ${TEST_WHEP_SOURCES})
    target_compile_definitions(test_whep_source PRIVATE HAS_WHEP=1)
    if(WIN32)
        target_compile_definitions(test_whep_source PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    target_include_directories(test_whep_source PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${stb_SOURCE_DIR}
    )
    if(FFMPEG_DIR)
        target_include_directories(test_whep_source PRIVATE "${FFMPEG_DIR}/include")
        target_link_directories(test_whep_source PRIVATE "${FFMPEG_DIR}/lib")
    endif()
    target_link_libraries(test_whep_source PRIVATE
        Catch2::Catch2WithMain
        LibDataChannel::LibDataChannel
        glad glm::glm OpenGL::GL
    )
    if(TARGET PkgConfig::FFMPEG)
        target_link_libraries(test_whep_source PRIVATE PkgConfig::FFMPEG)
    else()
        target_link_libraries(test_whep_source PRIVATE avformat avcodec avutil swscale swresample)
    endif()
    if(NOT WIN32)
        target_link_libraries(test_whep_source PRIVATE CURL::libcurl)
    endif()
    if(APPLE)
        target_link_libraries(test_whep_source PRIVATE
            "-framework WebKit" "-framework Cocoa" "-framework CoreFoundation"
        )
    endif()
endif()

if(HAS_NDI)
    # NDIRuntime.cpp/NDISource.cpp now call into NdiNetworkConfig (which uses
    # NetAdapters), so these test exes must compile + link the new modules and
    # the nlohmann include path. iphlpapi/ws2_32 auto-link via pragmas in
    # NetAdapters.cpp; macOS needs SystemConfiguration (added below).
    add_executable(test_ndi_receive
        tests/test_ndi_receive.cpp src/sources/NDIRuntime.cpp src/sources/NDISource.cpp
        src/net/NdiNetworkConfig.cpp src/app/NetAdapters.cpp
        src/render/Texture.cpp src/render/Framebuffer.cpp src/render/Mesh.cpp src/render/ShaderProgram.cpp
    )
    target_compile_definitions(test_ndi_receive PRIVATE HAS_NDI=1)
    if(WIN32)
        target_compile_definitions(test_ndi_receive PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    target_include_directories(test_ndi_receive PRIVATE ${CMAKE_SOURCE_DIR}/src ${NDI_DIR}/include ${stb_SOURCE_DIR})
    target_link_libraries(test_ndi_receive PRIVATE glfw glad glm::glm OpenGL::GL nlohmann_json::nlohmann_json)

    add_executable(test_ndi_capture tests/test_ndi_capture.cpp src/sources/NDIRuntime.cpp
        src/net/NdiNetworkConfig.cpp src/app/NetAdapters.cpp)
    target_compile_definitions(test_ndi_capture PRIVATE HAS_NDI=1)
    target_include_directories(test_ndi_capture PRIVATE ${CMAKE_SOURCE_DIR}/src ${NDI_DIR}/include)
    target_link_libraries(test_ndi_capture PRIVATE glfw glad OpenGL::GL nlohmann_json::nlohmann_json)

    add_executable(test_ndi_list tests/test_ndi_list.cpp src/sources/NDIRuntime.cpp
        src/net/NdiNetworkConfig.cpp src/app/NetAdapters.cpp)
    target_compile_definitions(test_ndi_list PRIVATE HAS_NDI=1)
    if(WIN32)
        target_compile_definitions(test_ndi_list PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    target_include_directories(test_ndi_list PRIVATE ${CMAKE_SOURCE_DIR}/src ${NDI_DIR}/include)
    target_link_libraries(test_ndi_list PRIVATE glfw glad OpenGL::GL nlohmann_json::nlohmann_json)

    if(APPLE)
        target_link_libraries(test_ndi_receive PRIVATE "-framework SystemConfiguration" "-framework CoreFoundation")
        target_link_libraries(test_ndi_capture PRIVATE "-framework SystemConfiguration" "-framework CoreFoundation")
        target_link_libraries(test_ndi_list    PRIVATE "-framework SystemConfiguration" "-framework CoreFoundation")
    endif()
endif()

if(WIN32)
    add_executable(test_wasapi_capture tests/test_wasapi_capture.cpp)
    target_compile_definitions(test_wasapi_capture PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(test_wasapi_capture PRIVATE ole32)

    add_executable(test_audio_e2e
        tests/test_audio_e2e.cpp src/app/AudioAnalyzer.cpp
        src/render/Texture.cpp src/render/Framebuffer.cpp src/render/Mesh.cpp src/render/ShaderProgram.cpp
        external/kiss_fft/kiss_fft.c
    )
    target_compile_definitions(test_audio_e2e PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(test_audio_e2e PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/external/kiss_fft ${stb_SOURCE_DIR}
    )
    target_link_libraries(test_audio_e2e PRIVATE glfw glad glm::glm OpenGL::GL ole32)
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders
)

# macOS: re-sign with the stable "Easel Dev" self-signed cert so the Mach-O
# CDHash is identical across rebuilds. TCC grants (Screen Recording / Mic /
# Camera) are keyed to CDHash, not path, so without this every rebuild looks
# like a brand-new app and re-prompts the user. The script falls back to
# ad-hoc signing if the cert is missing (scripts/setup-codesign.sh creates
# it; one-time interactive "Always Allow" in Keychain is required the first
# time codesign touches the private key).
if(APPLE)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/codesign-app.sh
                $<TARGET_BUNDLE_DIR:${PROJECT_NAME}>
        COMMENT "Codesigning Easel.app with 'Easel Dev' (stable TCC identity)"
    )
endif()

# Offline VideoRecorder audio-path harness — drives start()/stop() with no GL
# context (no GL call executes off the render path), so the WASAPI capture,
# mix, encode, and mux chain can be exercised without launching Easel.
if(FFMPEG_FOUND AND WIN32)
    add_executable(test_video_recorder tests/test_video_recorder.cpp src/app/VideoRecorder.cpp)
    target_compile_definitions(test_video_recorder PRIVATE HAS_FFMPEG=1 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(test_video_recorder PRIVATE ${CMAKE_SOURCE_DIR}/src "${FFMPEG_DIR}/include")
    target_link_directories(test_video_recorder PRIVATE "${FFMPEG_DIR}/lib")
    target_link_libraries(test_video_recorder PRIVATE glad avformat avcodec avutil swscale swresample ole32)
    add_custom_command(TARGET test_video_recorder POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:test_video_recorder>)
endif()
