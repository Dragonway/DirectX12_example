cmake_minimum_required(VERSION 3.21)

project(directx_example LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "DirectX is available on Windows only")
endif()

if(MSVC)
    # VS 2022 (MSVC 19.30+) supports up to C++20/C++23
    if(MSVC_VERSION GREATER_EQUAL "1930")
        message(STATUS "MSVC ${MSVC_VERSION} detected. Full C++20 and partial C++23/C++26 available.")
        set(CMAKE_CXX_STANDARD 20)

    # VS 2019 (MSVC 19.20 to 19.29)
    elseif(MSVC_VERSION GREATER_EQUAL "1920")
        message(STATUS "MSVC 2019 detected. C++17 and C++20 available.")
        set(CMAKE_CXX_STANDARD 17)

    # VS 2017 (MSVC 19.10 to 19.16)
    elseif(MSVC_VERSION GREATER_EQUAL "1910")
        message(STATUS "MSVC 2017 detected. C++14 and C++17 available.")
        set(CMAKE_CXX_STANDARD 14)
    endif()
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    DirectXHeaders
    GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers
    GIT_TAG main
)

FetchContent_MakeAvailable(DirectXHeaders)

# WIN32 -> GUI subsystem (/SUBSYSTEM:WINDOWS), entry point is wWinMain.
add_executable(directx_example WIN32 src/main.cpp)

target_compile_definitions(directx_example PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_precompile_headers(directx_example PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/pch.h")

if(MSVC)
    target_compile_options(directx_example PRIVATE /W4 /permissive- /utf-8)
endif()

target_link_libraries(directx_example PRIVATE
    Microsoft::DirectX-Headers

    # DirectX import libraries from the Windows SDK (<SDK>\Lib\<version>\um\x64)
    # The names are resolved by the linker through the SDK library path, which the Visual Studio generator (or vcvars64.bat for Ninja) sets up automatically
    d3d12
    dxgi
    d3dcompiler
)

target_include_directories(directx_example BEFORE PRIVATE "${DirectXHeaders_SOURCE_DIR}/include/directx")

add_custom_command(TARGET directx_example POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:directx_example>/shaders"
    COMMENT "Copying shader files to the executable directory..."
)

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT directx_example)

if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    message(STATUS "Windows SDK version: ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
endif()


file(GLOB_RECURSE SHADER_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsl")

add_custom_target(
    compile_shaders
    DEPENDS "${SHADER_FILES}"
)

foreach(SHADER_PATH ${SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER_PATH} NAME_WE)

    file(READ ${SHADER_PATH} SHADER_CONTENT)

    set(SHADERS_TO_CHECK
        "vs,vs_main,vs_6_0"
        "ps,ps_main,ps_6_0"
        "cs,cs_main,cs_6_0"
    )

    foreach(SHADER_TYPE ${SHADERS_TO_CHECK})
        string(REPLACE "," ";" SHADER_TYPE ${SHADER_TYPE})

        list(GET SHADER_TYPE 0 SUFFIX)
        list(GET SHADER_TYPE 1 ENTRY_POINT)
        list(GET SHADER_TYPE 2 TARGET_PROFILE)

        if (SHADER_CONTENT MATCHES "${ENTRY_POINT}")
            add_custom_command(
                TARGET compile_shaders POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:directx_example>/shaders"
                COMMAND dxc.exe -E ${ENTRY_POINT} -T ${TARGET_PROFILE} -Fo "$<TARGET_FILE_DIR:directx_example>/shaders/${SHADER_NAME}.${SUFFIX}.cso" "${SHADER_PATH}"
                COMMAND ${CMAKE_COMMAND} -E echo "DXC: compiling ${SHADER_NAME}.hlsl [${ENTRY_POINT} -> ${SHADER_NAME}.${SUFFIX}.cso]"
                VERBATIM
            )
        endif()
    endforeach()
endforeach()

add_dependencies(directx_example compile_shaders)
