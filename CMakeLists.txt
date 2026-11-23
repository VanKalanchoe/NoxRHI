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

set(NOXRHI_VMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/VulkanMemoryAllocator-Hpp" CACHE PATH "Path to VulkanMemoryAllocator-Hpp")
set(NOXRHI_D3D12MA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/D3D12MemoryAllocator" CACHE PATH "Path to D3D12MemoryAllocator")
set(NOXRHI_SLANG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/slang" CACHE PATH "Path to Slang SDK")
set(NOXRHI_STREAMLINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/Streamline" CACHE PATH "Path to NVIDIA Streamline SDK")
set(NOXRHI_NRD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendors/NRD" CACHE PATH "Path to NVIDIA NRD SDK")

if(WIN32 AND NRI_ENABLE_D3D12)
    file(GLOB NOXRHI_D3D12_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/D3D12/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/D3D12/*.h"
    )
    list(APPEND NOXRHI_CORE_SOURCES ${NOXRHI_D3D12_SOURCES})

    if(NOT TARGET D3D12MemoryAllocator AND EXISTS "${NOXRHI_D3D12MA_DIR}/src/D3D12MemAlloc.cpp")
        add_library(D3D12MemoryAllocator STATIC
            "${NOXRHI_D3D12MA_DIR}/src/D3D12MemAlloc.cpp"
        )
        target_include_directories(D3D12MemoryAllocator SYSTEM PUBLIC
            "${NOXRHI_D3D12MA_DIR}/include"
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
        "${NOXRHI_VMA_DIR}/include"
        "${NOXRHI_VMA_DIR}/VulkanMemoryAllocator/include"
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

if(NRI_ENABLE_SLANG)
    target_compile_definitions(NoxRHI PRIVATE NRI_ENABLE_SLANG=1)
    target_include_directories(NoxRHI SYSTEM PRIVATE
        "${NOXRHI_SLANG_DIR}/include"
    )
    if(WIN32)
        target_link_libraries(NoxRHI PRIVATE "${NOXRHI_SLANG_DIR}/lib/slang.lib")
        if(MSVC)
            if(NRI_STATIC_LIBRARY)
                target_link_options(NoxRHI INTERFACE "/DELAYLOAD:slang.dll")
                target_link_libraries(NoxRHI INTERFACE delayimp)
            else()
                target_link_options(NoxRHI PRIVATE "/DELAYLOAD:slang.dll")
                target_link_libraries(NoxRHI PRIVATE delayimp)
            endif()
        endif()
    else()
        # Linux / macOS: the prebuilt Slang release's shared library (libslang-compiler since 2026.x, libslang before).
        find_library(NOXRHI_SLANG_LIBRARY NAMES slang-compiler slang PATHS "${NOXRHI_SLANG_DIR}/lib" NO_DEFAULT_PATH)
        if(NOT NOXRHI_SLANG_LIBRARY)
            message(FATAL_ERROR "NRI_ENABLE_SLANG: no libslang-compiler / libslang in ${NOXRHI_SLANG_DIR}/lib")
        endif()
        target_link_libraries(NoxRHI PRIVATE "${NOXRHI_SLANG_LIBRARY}")
        set_target_properties(NoxRHI PROPERTIES BUILD_RPATH "${NOXRHI_SLANG_DIR}/lib")
    endif()
endif()

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
                # Since Slang 2026.x slang.dll only forwards its exports to slang-compiler.dll (a missing one fails the first
                # compile with delay-load error 0xC06D007F), and the compiler loads slang-glslang / slang-glsl-module itself:
                # copy every slang*.dll but slang-llvm.dll (84 MB, CPU targets only).
                file(GLOB SLANG_DLLS "${CMAKE_CURRENT_SOURCE_DIR}/vendors/slang/bin/slang*.dll")
                list(FILTER SLANG_DLLS EXCLUDE REGEX "slang-llvm\\.dll$")
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SLANG_DLLS} $<TARGET_FILE_DIR:${target_name}>
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

    # ShaderCompat: one Slang source on Vulkan and D3D12, every result checked (headless, no SDL).
    add_executable(ShaderCompat examples/ShaderCompat/main.cpp)
    target_link_libraries(ShaderCompat PRIVATE NoxRHI::HPP)
    noxrhi_copy_core_dll(ShaderCompat)
    add_custom_command(TARGET ShaderCompat POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/ShaderCompat/shader_compat.slang"
            $<TARGET_FILE_DIR:ShaderCompat>/shader_compat.slang
    )

    # examples/Common's shaders (nox_gpu.slang, nox_checks_selftest.slang) next to the executables, by one step that the examples depend
    # on: copied once, not by every example's step -- those ran in parallel into the same directory and collided on Windows ("cannot set
    # permissions"). The step lists the files when it runs (cmake/CopyExampleShaders.cmake), so a new file is copied by the next build.
    add_custom_target(noxrhi_example_common_shaders ALL
        COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/examples/Common" "-DDESTINATION=$<TARGET_FILE_DIR:ShaderCompat>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyExampleShaders.cmake"
        COMMENT "Copying examples/Common's shaders")
    add_dependencies(ShaderCompat noxrhi_example_common_shaders)

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

        # Cross-API examples: one Slang source each, run with --vk or --dx12 (examples/Common: the shared window / device / frame
        # loop and nox_gpu.slang, the one header that hides what differs between Vulkan and D3D12 shaders).
        function(noxrhi_add_cross_api_example name)
            add_executable(${name} examples/${name}/main.cpp examples/Common/ExampleApp.h examples/Common/ExampleMath.h)
            target_link_libraries(${name} PRIVATE NoxRHI::RenderGraph SDL3::SDL3)
            noxrhi_copy_runtime_deps(${name})
            # The example's own shaders, copied by a step that runs on every build and lists the files then (cmake/CopyExampleShaders.cmake):
            # a .slang file added to an example, or one edited without touching main.cpp, reaches the exe directory with the next build.
            # examples/Common's come from noxrhi_example_common_shaders.
            add_custom_target(${name}_shaders ALL
                COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/examples/${name}" "-DDESTINATION=$<TARGET_FILE_DIR:${name}>"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyExampleShaders.cmake"
                COMMENT "Copying ${name}'s shaders")
            add_dependencies(${name} ${name}_shaders noxrhi_example_common_shaders)
        endfunction()
        noxrhi_add_cross_api_example(GpuParticles)
        noxrhi_add_cross_api_example(BindlessScene)
        noxrhi_add_cross_api_example(MeshletCulling)
        noxrhi_add_cross_api_example(RayQueryShadows)
        noxrhi_add_cross_api_example(PathTracer)
        # PathTracer's NRD mode packs its inputs with NRD's own shader functions (NRD.hlsli + the generated NRDConfig.hlsli); its DLSS-RR
        # and NRD modes need the Streamline / NRD DLLs beside it.
        if(NOXRHI_HAS_NRD AND EXISTS "${NOXRHI_NRD_DIR}/Shaders/NRD.hlsli")
            target_compile_definitions(PathTracer PRIVATE NOXRHI_NRD_SHADER_DIR="${NOXRHI_NRD_DIR}/Shaders")
        endif()
        noxrhi_copy_streamline_nrd_deps(PathTracer)

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
