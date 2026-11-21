cmake_minimum_required(VERSION 3.20)
project(GKPRORecorder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: no redistributable needed, smaller memory footprint
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(GKPRORecorder WIN32
    src/main.cpp
    src/recorder.cpp
    src/recorder.h
    src/mkv_writer.h
)

target_compile_definitions(GKPRORecorder PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

target_link_libraries(GKPRORecorder PRIVATE
    d3d11 dxgi mfplat mfuuid strmiids ole32 oleaut32 shell32 user32 winmm
)

if(MSVC)
    target_compile_options(GKPRORecorder PRIVATE /O2 /GL /W3)
    target_link_options(GKPRORecorder PRIVATE /LTCG /OPT:REF /OPT:ICF)
endif()
