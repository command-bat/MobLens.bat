cmake_minimum_required(VERSION 3.16)
project(MobLens CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "MobLens é exclusivo para Windows")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
                        _WIN32_WINNT=0x0A00 WINVER=0x0A00)

# --- Câmera virtual: DLL carregada pelo serviço de câmera do Windows --------
add_library(moblens_vcam SHARED
    src/vcam/dllmain.cpp
    src/vcam/activator.cpp
    src/vcam/media_source.cpp
    src/vcam/media_stream.cpp
    src/vcam/frame_reader.cpp
    src/vcam/moblens_vcam.def
)
target_link_libraries(moblens_vcam PRIVATE mfplat mfuuid ole32 advapi32)
set_target_properties(moblens_vcam PROPERTIES PREFIX "")

# --- Webcam DirectShow (Windows 10/8/7): carregada pelos aplicativos ------
add_library(moblens_dshow SHARED
    src/dshow/dshow_filter.cpp
    src/dshow/moblens_dshow.def
)
target_link_libraries(moblens_dshow PRIVATE strmiids uuid ole32 advapi32)
set_target_properties(moblens_dshow PROPERTIES PREFIX "")

# --- Aplicativo (janela + bandeja) ---------------------------------------------------
add_executable(MobLens WIN32
    src/app/main.cpp
    src/app/installer.cpp
    src/app/streamer.cpp
    src/app/adb.cpp
    src/app/h264_decoder.cpp
    src/app/nv12.cpp
    src/app/idle_frame.cpp
    src/app/log.cpp
    src/app/theme.cpp
    src/app/moblens.rc
)
# o .rc referencia o manifesto pelo nome
target_include_directories(MobLens PRIVATE src/app)
target_link_libraries(MobLens PRIVATE mfplat mfuuid ole32 advapi32 shell32
                      user32 gdi32 ws2_32 comctl32 dwmapi uxtheme)

if(MINGW)
    # Sem dependência das DLLs do MSYS2 (libstdc++, libgcc, winpthread)
    foreach(t moblens_vcam moblens_dshow MobLens)
        target_link_options(${t} PRIVATE -static -static-libgcc
                            -static-libstdc++)
    endforeach()
    target_link_options(MobLens PRIVATE -municode)
elseif(MSVC)
    set_property(TARGET moblens_vcam moblens_dshow MobLens PROPERTY
                 MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
