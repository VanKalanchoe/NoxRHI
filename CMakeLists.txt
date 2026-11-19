cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendors/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vendors/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
endif()

project(NoxRHI VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_VS_GLOBALS "UseMultiToolTask=true" "EnforceProcessCountAcrossBuilds=true")
endif()
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/MP>)
endif()

option(NRI_STATIC_LIBRARY    "Build NoxRHI core as a static library instead of a shared DLL" OFF)
option(NRI_BUILD_HPP         "Provide header-only C++20 NRIHPP target" ON)
option(NRI_BUILD_RENDERGRAPH "Build NRIRenderGraph target on top of NRIHPP" ON)
option(NRI_ENABLE_VULKAN     "Enable Vulkan backend" ON)
option(NRI_ENABLE_D3D12      "Enable D3D12 backend" ON)
option(NRI_ENABLE_NONE       "Enable No-GPU / Dedicated Server (None) backend" ON)
option(NRI_ENABLE_SLANG      "Enable Slang shader compiler extension" ON)
option(NRI_ENABLE_STREAMLINE "Enable NVIDIA Streamline (DLSS-SR / DLSS-RR) extension" ON)
option(NRI_ENABLE_NRD        "Enable NVIDIA NRD (SIGMA / REBLUR / RELAX) denoiser extension" ON)
option(NRI_ENABLE_PCH        "Enable private precompiled header for faster NoxRHI builds" ON)
option(NRI_BUILD_EXAMPLES    "Build standalone NoxRHI examples" ON)

if(NRI_BUILD_RENDERGRAPH AND NOT NRI_BUILD_HPP)
    message(FATAL_ERROR "NRI_BUILD_RENDERGRAPH requires NRI_BUILD_HPP=ON")
endif()

# -----------------------------------------------------------------------------
# Layer 1 & 2: NoxRHI Core + Extensions (C ABI boundary)
# -----------------------------------------------------------------------------

set(NOXRHI_CORE_SOURCES
    src/Core/CInterface.cpp
    src/Core/Device.cpp
    src/Extensions/Wrapper/WrapperVK.cpp
    src/Extensions/Wrapper/WrapperD3D12.cpp
    src/Extensions/Imgui/ImguiExtension.cpp
    src/Extensions/Upscaler/StreamlineContext.cpp
    src/Extensions/Upscaler/UpscalerExtension.cpp
    src/Extensions/Denoiser/NrdIntegrationHost.cpp
    src/Extensions/Denoiser/DenoiserExtension.cpp
)

if(NRI_ENABLE_VULKAN)
    find_package(Vulkan REQUIRED)
    file(GLOB NOXRHI_VK_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Vulkan/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Vulkan/*.h"
    )
    list(APPEND NOXRHI_CORE_SOURCES ${NOXRHI_VK_SOURCES})
endif()

if(WIN32 AND NRI_ENABLE_D3D12)
    file(GLOB NOXRHI_D3D12_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/D3D12/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/D3D12/*.h"
    )
    list(APPEND NOXRHI_CORE_SOURCES ${NOXRHI_D3D12_SOURCES})

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendors/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp")
        add_library(D3D12MemoryAllocator STATIC
            "${CMAKE_CURRENT_SOURCE_DIR}/vendors/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp"
        )
        target_include_directories(D3D12MemoryAllocator SYSTEM PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/vendors/D3D12MemoryAllocator/include"
        )
        target_link_libraries(D3D12MemoryAllocator PUBLIC d3d12 dxgi)
    endif()
endif()

if(NRI_ENABLE_NONE)
    list(APPEND NOXRHI_CORE_SOURCES
        src/Core/None/DeviceNone.cpp
        src/Core/None/DeviceNone.h
    )
endif()

if(NRI_ENABLE_SLANG)
    list(APPEND NOXRHI_CORE_SOURCES
        src/Extensions/ShaderCompiler/SlangCompiler.cpp
    )
endif()

if(NRI_STATIC_LIBRARY)
    add_library(NoxRHI STATIC ${NOXRHI_CORE_SOURCES})
    target_compile_definitions(NoxRHI PUBLIC NRI_STATIC_LIBRARY)
else()
    add_library(NoxRHI SHARED ${NOXRHI_CORE_SOURCES})
endif()
add_library(NoxRHI::Core ALIAS NoxRHI)

target_compile_definitions(NoxRHI PRIVATE
    NRI_EXPORTS
    $<$<BOOL:${WIN32}>:NOMINMAX>
    $<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN>
)
target_include_directories(NoxRHI
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core"
)

if(NRI_ENABLE_VULKAN)
    target_compile_definitions(NoxRHI PRIVATE
        NRI_ENABLE_VULKAN=1
        $<$<BOOL:${WIN32}>:VK_USE_PLATFORM_WIN32_KHR>
        VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
        VULKAN_HPP_NO_STRUCT_CONSTRUCTORS=1
        VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS
    )
    target_include_directories(NoxRHI SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendors/VulkanMemoryAllocator-Hpp/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendors/VulkanMemoryAllocator-Hpp/VulkanMemoryAllocator/include"
    )
    target_link_libraries(NoxRHI PRIVATE Vulkan::Vulkan)
endif()

if(WIN32 AND NRI_ENABLE_D3D12)
    target_compile_definitions(NoxRHI PRIVATE NRI_ENABLE_D3D12=1)
    target_link_libraries(NoxRHI PRIVATE d3d12 dxgi dxguid)
    if(TARGET D3D12MemoryAllocator)
        target_link_libraries(NoxRHI PRIVATE D3D12MemoryAllocator)
    endif()
endif()

if(NRI_ENABLE_NONE)
    target_compile_definitions(NoxRHI PRIVATE NRI_ENABLE_NONE=1)
endif()

if(NRI_ENABLE_NONE)
    list(APPEND NOXRHI_CORE_SOURCES
        src/Core/None/DeviceNone.cpp
        src/Core/None/DeviceNone.h
    )
endif()

if(NRI_ENABLE_SLANG)
    target_compile_definitions(NoxRHI PRIVATE NRI_ENABLE_SLANG=1)
    target_include_directories(NoxRHI SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendors/slang/include"
    )
    if(WIN32)
        target_link_libraries(NoxRHI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendors/slang/lib/slang.lib")
        if(MSVC)
            if(NRI_STATIC_LIBRARY)
                target_link_options(NoxRHI INTERFACE "/DELAYLOAD:slang.dll")
                target_link_libraries(NoxRHI INTERFACE delayimp)
            else()
                target_link_options(NoxRHI PRIVATE "/DELAYLOAD:slang.dll")
                target_link_libraries(NoxRHI PRIVATE delayimp)
            endif()
        endif()
    endif()
endif()

set(NOXRHI_STREAMLINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/Streamline")
set(NOXRHI_HAS_STREAMLINE OFF)
if(NRI_ENABLE_STREAMLINE AND EXISTS "${NOXRHI_STREAMLINE_DIR}/include/sl.h" AND EXISTS "${NOXRHI_STREAMLINE_DIR}/lib/x64/sl.interposer.lib")
    set(NOXRHI_HAS_STREAMLINE ON)
    target_compile_definitions(NoxRHI PRIVATE NRI_ENABLE_STREAMLINE=1)
    target_include_directories(NoxRHI SYSTEM PRIVATE "${NOXRHI_STREAMLINE_DIR}/include")
    if(WIN32)
        target_link_libraries(NoxRHI PRIVATE "${NOXRHI_STREAMLINE_DIR}/lib/x64/sl.interposer.lib")
        if(MSVC)
            if(NRI_STATIC_LIBRARY)
                target_link_options(NoxRHI INTERFACE "/DELAYLOAD:sl.interposer.dll")
                target_link_libraries(NoxRHI INTERFACE delayimp)
            else()
                target_link_options(NoxRHI PRIVATE "/DELAYLOAD:sl.interposer.dll")
                target_link_libraries(NoxRHI PRIVATE delayimp)
            endif()
        endif()
    endif()
endif()

set(NOXRHI_NRD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/NRD")
set(NOXRHI_HAS_NRD OFF)
if(NRI_ENABLE_NRD AND EXISTS "${NOXRHI_NRD_DIR}/Include/NRD.h" AND EXISTS "${NOXRHI_NRD_DIR}/Integration/NRDIntegration.hpp")
    set(NOXRHI_HAS_NRD ON)
    target_compile_definitions(NoxRHI PRIVATE NRI_ENABLE_NRD=1)
    target_include_directories(NoxRHI SYSTEM PRIVATE
        "${NOXRHI_NRD_DIR}/Include"
        "${NOXRHI_NRD_DIR}/Integration"
    )
    if(WIN32)
        target_link_libraries(NoxRHI PRIVATE
            $<$<CONFIG:Debug>:${NOXRHI_NRD_DIR}/Lib/Debug/NRD.lib>
            $<$<NOT:$<CONFIG:Debug>>:${NOXRHI_NRD_DIR}/Lib/Release/NRD.lib>
        )
        if(MSVC)
            if(NRI_STATIC_LIBRARY)
                target_link_options(NoxRHI INTERFACE "/DELAYLOAD:NRD.dll")
                target_link_libraries(NoxRHI INTERFACE delayimp)
            else()
                target_link_options(NoxRHI PRIVATE "/DELAYLOAD:NRD.dll")
                target_link_libraries(NoxRHI PRIVATE delayimp)
            endif()
        endif()
    endif()
endif()

if(NRI_ENABLE_PCH)
    target_precompile_headers(NoxRHI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/NoxRHIPCH.h")
    set_source_files_properties(src/Extensions/Denoiser/NrdIntegrationHost.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

# -----------------------------------------------------------------------------
# Layer 3: NoxRHI::HPP (Header-only C++20 RAII wrapper)
# -----------------------------------------------------------------------------

if(NRI_BUILD_HPP)
    add_library(NoxRHI_HPP INTERFACE)
    add_library(NoxRHI::HPP ALIAS NoxRHI_HPP)
    target_include_directories(NoxRHI_HPP INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(NoxRHI_HPP INTERFACE NoxRHI)
    target_compile_features(NoxRHI_HPP INTERFACE cxx_std_20)
endif()

# -----------------------------------------------------------------------------
# Layer 4: NoxRHI::RenderGraph (Standalone C++20 RenderGraph)
# -----------------------------------------------------------------------------

if(NRI_BUILD_RENDERGRAPH)
    add_library(NoxRHI_RenderGraph STATIC
        src/RenderGraph/RenderGraph.cpp
        src/RenderGraph/RGResourcePool.cpp
        src/RenderGraph/RGFrameReport.cpp
        src/RenderGraph/RGValidation.cpp
    )
    add_library(NoxRHI::RenderGraph ALIAS NoxRHI_RenderGraph)
    target_link_libraries(NoxRHI_RenderGraph PUBLIC NoxRHI::HPP)
    target_compile_features(NoxRHI_RenderGraph PUBLIC cxx_std_20)
endif()

# -----------------------------------------------------------------------------
# Standalone Examples (SDL3 strictly isolated here)
# -----------------------------------------------------------------------------

if(NRI_BUILD_EXAMPLES)
    find_package(SDL3 CONFIG REQUIRED)

    function(noxrhi_copy_core_dll target_name)
        if(WIN32 AND NOT NRI_STATIC_LIBRARY)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:NoxRHI>
                    $<TARGET_FILE_DIR:${target_name}>
            )
        endif()
    endfunction()

    function(noxrhi_copy_streamline_nrd_deps target_name)
        if(WIN32)
            if(NOXRHI_HAS_STREAMLINE)
                file(GLOB STREAMLINE_DEBUG_DLLS "${NOXRHI_STREAMLINE_DIR}/bin/x64/development/*.dll")
                file(GLOB STREAMLINE_RELEASE_DLLS "${NOXRHI_STREAMLINE_DIR}/bin/x64/*.dll")
                foreach(SL_DLL ${STREAMLINE_DEBUG_DLLS})
                    add_custom_command(TARGET ${target_name} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E $<$<CONFIG:Debug>:copy_if_different>$<$<NOT:$<CONFIG:Debug>>:true>
                            "${SL_DLL}"
                            $<TARGET_FILE_DIR:${target_name}>
                    )
                endforeach()
                foreach(SL_DLL ${STREAMLINE_RELEASE_DLLS})
                    add_custom_command(TARGET ${target_name} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E $<$<NOT:$<CONFIG:Debug>>:copy_if_different>$<$<CONFIG:Debug>:true>
                            "${SL_DLL}"
                            $<TARGET_FILE_DIR:${target_name}>
                    )
                endforeach()
            endif()
            if(NOXRHI_HAS_NRD)
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<IF:$<CONFIG:Debug>,${NOXRHI_NRD_DIR}/Lib/Debug/NRD.dll,${NOXRHI_NRD_DIR}/Lib/Release/NRD.dll>"
                        "$<IF:$<CONFIG:Debug>,${NOXRHI_NRD_DIR}/Lib/Debug/NRI.dll,${NOXRHI_NRD_DIR}/Lib/Release/NRI.dll>"
                        $<TARGET_FILE_DIR:${target_name}>
                )
            endif()
        endif()
    endfunction()

    function(noxrhi_copy_runtime_deps target_name)
        noxrhi_copy_core_dll(${target_name})
        if(WIN32)
            if(NRI_ENABLE_SLANG AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendors/slang/bin/slang.dll")
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${CMAKE_CURRENT_SOURCE_DIR}/vendors/slang/bin/slang.dll"
                        $<TARGET_FILE_DIR:${target_name}>
                )
                file(GLOB SLANG_STD_MODULES LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/vendors/slang/bin/slang-standard-module-*")
                foreach(SLANG_MOD_DIR ${SLANG_STD_MODULES})
                    get_filename_component(SLANG_MOD_NAME "${SLANG_MOD_DIR}" NAME)
                    add_custom_command(TARGET ${target_name} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                            "${SLANG_MOD_DIR}"
                            "$<TARGET_FILE_DIR:${target_name}>/${SLANG_MOD_NAME}"
                    )
                endforeach()
            endif()
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:SDL3::SDL3>
                    $<TARGET_FILE_DIR:${target_name}>
            )
        endif()
    endfunction()

    add_executable(HeadlessRender examples/HeadlessRender/main.cpp)
    target_link_libraries(HeadlessRender PRIVATE NoxRHI::HPP)
    noxrhi_copy_core_dll(HeadlessRender)

    if(NRI_BUILD_RENDERGRAPH)
        add_executable(HeadlessNone examples/HeadlessNone/main.cpp)
        target_link_libraries(HeadlessNone PRIVATE NoxRHI::RenderGraph)
        noxrhi_copy_core_dll(HeadlessNone)
    endif()

    add_executable(StreamlineNRDDemo examples/StreamlineNRDDemo/main.cpp)
    target_link_libraries(StreamlineNRDDemo PRIVATE NoxRHI::HPP)
    noxrhi_copy_core_dll(StreamlineNRDDemo)
    noxrhi_copy_streamline_nrd_deps(StreamlineNRDDemo)

    add_executable(ClearScreen examples/ClearScreen/main.cpp)
    target_link_libraries(ClearScreen PRIVATE NoxRHI::HPP SDL3::SDL3)
    noxrhi_copy_runtime_deps(ClearScreen)

    if(NRI_BUILD_RENDERGRAPH)
        add_executable(RenderGraphTriangle examples/RenderGraphTriangle/main.cpp)
        target_link_libraries(RenderGraphTriangle PRIVATE NoxRHI::RenderGraph SDL3::SDL3)
        noxrhi_copy_runtime_deps(RenderGraphTriangle)
        add_custom_command(TARGET RenderGraphTriangle POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/RenderGraphTriangle/triangle.slang"
                $<TARGET_FILE_DIR:RenderGraphTriangle>/triangle.slang
        )

        find_package(Stb QUIET)
        if(Stb_FOUND OR DEFINED Stb_INCLUDE_DIR)
            add_executable(TexturedQuads examples/TexturedQuads/main.cpp)
            target_include_directories(TexturedQuads PRIVATE ${Stb_INCLUDE_DIR})
            target_link_libraries(TexturedQuads PRIVATE NoxRHI::RenderGraph SDL3::SDL3)
            noxrhi_copy_runtime_deps(TexturedQuads)
            add_custom_command(TARGET TexturedQuads POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/examples/TexturedQuads/textured_quads.slang"
                    $<TARGET_FILE_DIR:TexturedQuads>/textured_quads.slang
                COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/examples/TexturedQuads/assets"
                    "$<TARGET_FILE_DIR:TexturedQuads>/assets"
            )
        endif()

        set(NOXRHI_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/imgui")
        if(EXISTS "${NOXRHI_IMGUI_DIR}/imgui.h")
            set(NOXRHI_IMGUI_SOURCES
                "${NOXRHI_IMGUI_DIR}/imgui.cpp"
                "${NOXRHI_IMGUI_DIR}/imgui_demo.cpp"
                "${NOXRHI_IMGUI_DIR}/imgui_draw.cpp"
                "${NOXRHI_IMGUI_DIR}/imgui_tables.cpp"
                "${NOXRHI_IMGUI_DIR}/imgui_widgets.cpp"
                "${NOXRHI_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
            )

            add_executable(ImGuiDemo examples/ImGuiDemo/main.cpp ${NOXRHI_IMGUI_SOURCES})
            target_include_directories(ImGuiDemo SYSTEM PRIVATE
                "${NOXRHI_IMGUI_DIR}"
                "${NOXRHI_IMGUI_DIR}/backends"
            )
            target_link_libraries(ImGuiDemo PRIVATE NoxRHI::RenderGraph SDL3::SDL3)
            noxrhi_copy_runtime_deps(ImGuiDemo)
        endif()
    endif()
endif()
