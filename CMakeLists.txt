cmake_minimum_required(VERSION 3.20)
project(Easel VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch all dependencies
include(cmake/FetchDependencies.cmake)

# FFmpeg (external pre-built)
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/external/ffmpeg-master-latest-win64-gpl-shared")
if(EXISTS "${FFMPEG_DIR}")
    set(FFMPEG_FOUND TRUE)
    message(STATUS "FFmpeg found at ${FFMPEG_DIR}")
else()
    set(FFMPEG_FOUND FALSE)
    message(WARNING "FFmpeg not found at ${FFMPEG_DIR} - video support disabled")
endif()

# OpenCV (external pre-built, same pattern as FFmpeg)
set(OPENCV_DIR "${CMAKE_SOURCE_DIR}/external/opencv")
if(EXISTS "${OPENCV_DIR}")
    set(OpenCV_DIR "${OPENCV_DIR}/x64/vc16/lib")
    find_package(OpenCV QUIET COMPONENTS core imgproc calib3d videoio)
    if(OpenCV_FOUND)
        set(HAS_OPENCV TRUE)
        message(STATUS "OpenCV ${OpenCV_VERSION} found at ${OPENCV_DIR}")
    else()
        set(HAS_OPENCV FALSE)
        message(WARNING "OpenCV found at ${OPENCV_DIR} but find_package failed - scanning disabled")
    endif()
else()
    set(HAS_OPENCV FALSE)
    message(STATUS "OpenCV not found at ${OPENCV_DIR} - scanning disabled")
endif()

# Whisper.cpp disabled (CUDA compilation too slow)
set(HAS_WHISPER FALSE)
message(STATUS "whisper.cpp disabled")

# NDI (headers only - runtime loaded dynamically)
set(NDI_DIR "${CMAKE_SOURCE_DIR}/external/ndi")
if(EXISTS "${NDI_DIR}/include/Processing.NDI.Lib.h")
    set(HAS_NDI TRUE)
    message(STATUS "NDI SDK headers found at ${NDI_DIR}")
else()
    set(HAS_NDI FALSE)
    message(STATUS "NDI SDK headers not found at ${NDI_DIR} - NDI support disabled")
endif()

# Collect source files
file(GLOB_RECURSE SOURCES
    src/*.cpp
    src/*.h
)

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
# Exclude old WebView2 SpeechBridge (replaced by EthereaTranscript)
list(FILTER SOURCES EXCLUDE REGEX ".*SpeechBridge.*")
if(NOT HAS_NDI)
    list(FILTER SOURCES EXCLUDE REGEX ".*NDI.*")
endif()

# WHEP disabled — WebRTC NAT traversal to remote pods unreliable without server-side TURN
set(HAS_WHEP FALSE)
list(FILTER SOURCES EXCLUDE REGEX ".*WHEP.*")
message(STATUS "WHEP source disabled")

# kiss_fft (public domain FFT library for AudioAnalyzer)
set(KISS_FFT_SRC "${CMAKE_SOURCE_DIR}/external/kiss_fft/kiss_fft.c")

add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${KISS_FFT_SRC})

# Shader test harness (console app)
add_executable(test_shaders
    src/test_shaders.cpp
    src/sources/ShaderSource.cpp
    src/render/ShaderProgram.cpp
    src/render/Framebuffer.cpp
    src/render/Mesh.cpp
    src/render/Texture.cpp
)
target_compile_definitions(test_shaders PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(test_shaders PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${stb_SOURCE_DIR}
)
target_link_libraries(test_shaders PRIVATE
    glfw glad glm::glm nlohmann_json::nlohmann_json opengl32
)

# Prevent Windows min/max macros from conflicting with std::min/std::max
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

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
    glfw
    glad
    glm::glm
    imgui
    nlohmann_json::nlohmann_json
    opengl32
    dwmapi
    d3d11
    dxgi
    comdlg32
    ws2_32
    runtimeobject
)

# FFmpeg linking
if(FFMPEG_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE "${FFMPEG_DIR}/include")
    target_link_directories(${PROJECT_NAME} PRIVATE "${FFMPEG_DIR}/lib")
    target_link_libraries(${PROJECT_NAME} PRIVATE
        avformat
        avcodec
        avutil
        swscale
        swresample
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_FFMPEG=1)

    # Copy FFmpeg DLLs to output directory
    file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FFMPEG_DLLS}
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )
endif()

# OpenCV linking
if(HAS_OPENCV)
    target_include_directories(${PROJECT_NAME} PRIVATE "${OPENCV_DIR}/include")
    target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenCV_LIBS})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_OPENCV=1)

    # Copy OpenCV DLLs to output directory
    file(GLOB OPENCV_DLLS "${OPENCV_DIR}/x64/vc16/bin/*.dll")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${OPENCV_DLLS}
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )
endif()

# Whisper.cpp linking
if(HAS_WHISPER)
    target_link_libraries(${PROJECT_NAME} PRIVATE whisper)
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/external")
    target_include_directories(${PROJECT_NAME} PRIVATE "${whisper_SOURCE_DIR}/include")
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_WHISPER=1)
endif()

# NDI (headers only, runtime loaded dynamically - no link libraries needed)
if(HAS_NDI)
    target_include_directories(${PROJECT_NAME} PRIVATE "${NDI_DIR}/include")
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_NDI=1)
endif()


# Scanning test suite (requires OpenCV)
if(HAS_OPENCV)
    add_executable(test_scanning
        tests/test_scanning.cpp
        src/scanning/GrayCodePattern.cpp
        src/scanning/SceneScanner.cpp
        src/scanning/WebcamSource.cpp
        src/render/Texture.cpp
        src/render/Framebuffer.cpp
        src/render/Mesh.cpp
        src/render/ShaderProgram.cpp
    )
    target_compile_definitions(test_scanning PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN HAS_OPENCV=1)
    target_include_directories(test_scanning PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${stb_SOURCE_DIR}
        "${OPENCV_DIR}/include"
    )
    target_link_libraries(test_scanning PRIVATE
        Catch2::Catch2WithMain
        ${OpenCV_LIBS}
        glfw glad glm::glm nlohmann_json::nlohmann_json opengl32
    )

    # Copy OpenCV DLLs for test executable
    add_custom_command(TARGET test_scanning POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${OPENCV_DLLS}
        $<TARGET_FILE_DIR:test_scanning>
    )

    # Register with CTest
    include(CTest)
    include(Catch)
    catch_discover_tests(test_scanning)
endif()

# AudioAnalyzer test suite
add_executable(test_audio_analyzer
    tests/test_audio_analyzer.cpp
    src/app/AudioAnalyzer.cpp
    src/render/Texture.cpp
    src/render/Framebuffer.cpp
    src/render/Mesh.cpp
    src/render/ShaderProgram.cpp
    external/kiss_fft/kiss_fft.c
)
target_compile_definitions(test_audio_analyzer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(test_audio_analyzer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/kiss_fft
    ${stb_SOURCE_DIR}
)
target_link_libraries(test_audio_analyzer PRIVATE
    Catch2::Catch2WithMain
    glfw glad glm::glm nlohmann_json::nlohmann_json opengl32
)

# Copy shaders to build directory
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/shaders
    $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders
)
