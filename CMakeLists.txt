cmake_minimum_required(VERSION 3.20)
project(GKPRORecorder LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: no redistributable needed, smaller memory footprint (also for the injected hook DLL)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The 32-bit build only needs the game-capture hook + injector (for 32-bit games).
option(GKPRO_HOOK_ONLY "Build only the game-capture hook" OFF)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(GK_BITS 64)
else()
    set(GK_BITS 32)
endif()

# ---------------------------------------------------------------- game-capture hook (injected into games)
set(MH ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/minhook)
add_library(gkhook SHARED
    src/hook/gkhook.cpp
    ${MH}/src/buffer.c
    ${MH}/src/hook.c
    ${MH}/src/trampoline.c
    ${MH}/src/hde/hde32.c
    ${MH}/src/hde/hde64.c
)
target_include_directories(gkhook PRIVATE ${MH}/include)
target_compile_definitions(gkhook PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(gkhook PRIVATE user32 kernel32)
set_target_properties(gkhook PROPERTIES OUTPUT_NAME gkhook${GK_BITS} PREFIX "")

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_executable(gkinject32 WIN32 src/hook/inject32.cpp)
    target_compile_definitions(gkinject32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(gkinject32 PRIVATE shell32 user32 kernel32)
endif()

# ---------------------------------------------------------------- the recorder itself (64-bit)
if(NOT GKPRO_HOOK_ONLY)
    add_executable(GKPRORecorder WIN32
        src/main.cpp
        src/recorder.cpp
        src/recorder.h
        src/mkv_writer.h
        src/wgc_capture.cpp
        src/wgc_capture.h
        src/audio_capture.cpp
        src/audio_capture.h
        src/mp4_remux.cpp
        src/mp4_remux.h
        src/hook_capture.cpp
        src/hook_capture.h
        src/hook_shared.h
    )

    target_compile_definitions(GKPRORecorder PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)

    target_link_libraries(GKPRORecorder PRIVATE
        d3d11 dxgi mfplat mfuuid strmiids ole32 oleaut32 shell32 user32 winmm avrt windowsapp
    )

    if(MSVC)
        target_compile_options(GKPRORecorder PRIVATE /O2 /GL /W3 /EHsc /Zc:__cplusplus)
        target_link_options(GKPRORecorder PRIVATE /LTCG /OPT:REF /OPT:ICF)
    endif()
endif()
