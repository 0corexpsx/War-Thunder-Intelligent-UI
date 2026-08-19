cmake_minimum_required(VERSION 3.21)
project(wt_second_screen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

add_executable(wt_second_screen WIN32
    src/main.cpp
    src/wt_client.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(wt_second_screen PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(wt_second_screen PRIVATE
    d3d11 d3dcompiler dxgi winhttp imm32 dwmapi shell32
)

target_compile_definitions(wt_second_screen PRIVATE
    UNICODE _UNICODE NOMINMAX
)

if(MSVC)
    target_compile_options(wt_second_screen PRIVATE /utf-8 /W3)
endif()
