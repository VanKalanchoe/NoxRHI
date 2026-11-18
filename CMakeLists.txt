cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendors/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vendors/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
endif()

project(NoxRHI VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NRI_STATIC_LIBRARY    "Build NoxRHI core as a static library instead of a shared DLL" OFF)
option(NRI_BUILD_HPP         "Provide header-only C++20 NRIHPP target" ON)
option(NRI_BUILD_RENDERGRAPH "Build NRIRenderGraph target on top of NRIHPP" ON)
option(NRI_ENABLE_VULKAN     "Enable Vulkan backend" ON)
option(NRI_ENABLE_D3D12      "Enable D3D12 backend" ON)
option(NRI_ENABLE_SLANG      "Enable Slang shader compiler extension" ON)
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

if(NRI_ENABLE_PCH)
    target_precompile_headers(NoxRHI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/NoxRHIPCH.h")
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

    function(noxrhi_copy_runtime_deps target_name)
        if(WIN32)
            if(NOT NRI_STATIC_LIBRARY)
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:NoxRHI>
                        $<TARGET_FILE_DIR:${target_name}>
                )
            endif()
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
            if(NRI_ENABLE_VULKAN)
                list(APPEND NOXRHI_IMGUI_SOURCES "${NOXRHI_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp")
            endif()
            if(WIN32 AND NRI_ENABLE_D3D12)
                list(APPEND NOXRHI_IMGUI_SOURCES "${NOXRHI_IMGUI_DIR}/backends/imgui_impl_dx12.cpp")
            endif()

            add_executable(ImGuiDemo examples/ImGuiDemo/main.cpp ${NOXRHI_IMGUI_SOURCES})
            target_include_directories(ImGuiDemo SYSTEM PRIVATE
                "${NOXRHI_IMGUI_DIR}"
                "${NOXRHI_IMGUI_DIR}/backends"
            )
            target_link_libraries(ImGuiDemo PRIVATE NoxRHI::RenderGraph SDL3::SDL3)
            if(NRI_ENABLE_VULKAN)
                target_compile_definitions(ImGuiDemo PRIVATE NRI_ENABLE_VULKAN=1)
                target_link_libraries(ImGuiDemo PRIVATE Vulkan::Vulkan)
            endif()
            if(WIN32 AND NRI_ENABLE_D3D12)
                target_compile_definitions(ImGuiDemo PRIVATE NRI_ENABLE_D3D12=1)
                target_link_libraries(ImGuiDemo PRIVATE d3d12 dxgi)
            endif()
            noxrhi_copy_runtime_deps(ImGuiDemo)
        endif()
    endif()
endif()
