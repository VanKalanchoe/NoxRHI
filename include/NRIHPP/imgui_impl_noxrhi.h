#pragma once

// Standard Dear ImGui renderer backend built on NoxRHI::HPP (NRIImgui extension).
// Requires <imgui.h> (1.92+ with ImTextureData) and <NRIHPP/NRIExtensions.hpp>.

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <NRIHPP/NRI.hpp>
#include <NRIHPP/NRIExtensions.hpp>

static_assert(sizeof(ImDrawVert) == sizeof(NriImguiVertex), "ImDrawVert must match NriImguiVertex size");
static_assert(offsetof(ImDrawVert, pos) == offsetof(NriImguiVertex, pos), "ImDrawVert::pos mismatch");
static_assert(offsetof(ImDrawVert, uv) == offsetof(NriImguiVertex, uv), "ImDrawVert::uv mismatch");
static_assert(offsetof(ImDrawVert, col) == offsetof(NriImguiVertex, col), "ImDrawVert::col mismatch");

struct ImGui_ImplNoxRHI_InitInfo
{
    nri::Device* device = nullptr;
    nri::DescriptorHeap* resourceHeap = nullptr; // Optional shared resource heap; nullptr allocates an internal heap
    nri::DescriptorHeap* samplerHeap = nullptr;  // Optional shared sampler heap; nullptr allocates an internal linear sampler heap
    uint32_t samplerSlot = 0;
    NriImageFormat colorFormat = NRI_FORMAT_SURFACE;
    uint32_t framesInFlight = 2;
    bool linearColorOutput = true;
    bool enableViewports = true;
};

namespace detail
{
    struct ImGuiNoxRHIScratch
    {
        std::vector<NriImguiDrawList> drawLists;
        std::vector<NriImguiDrawCmd> drawCmds;
    };

    struct ImGuiNoxRHIData
    {
        nri::Imgui imgui;
        ImGuiNoxRHIScratch mainScratch{};
        ImGuiNoxRHIScratch viewportScratch{};
        bool viewportsEnabled = false;
    };

    inline ImGuiNoxRHIData* getImGuiNoxRHIData()
    {
        return ImGui::GetCurrentContext()
            ? static_cast<ImGuiNoxRHIData*>(ImGui::GetIO().BackendRendererUserData)
            : nullptr;
    }

    inline void syncImGuiNoxRHITextures(nri::Imgui& imgui, ImDrawData* drawData)
    {
        if (!drawData || !drawData->Textures)
            return;

        for (ImTextureData* tex : *drawData->Textures)
        {
            if (tex->Status == ImTextureStatus_WantCreate)
            {
                const uint64_t texId = imgui.createTexture(static_cast<uint32_t>(tex->Width),
                                                           static_cast<uint32_t>(tex->Height),
                                                           tex->GetPixels());
                tex->SetTexID(static_cast<ImTextureID>(texId));
                tex->SetStatus(ImTextureStatus_OK);
            }
            else if (tex->Status == ImTextureStatus_WantUpdates)
            {
                imgui.updateTexture(static_cast<uint64_t>(tex->GetTexID()),
                                     static_cast<uint32_t>(tex->UpdateRect.x),
                                     static_cast<uint32_t>(tex->UpdateRect.y),
                                     static_cast<uint32_t>(tex->UpdateRect.w),
                                     static_cast<uint32_t>(tex->UpdateRect.h),
                                     tex->GetPixels(),
                                     static_cast<uint32_t>(tex->Width),
                                     static_cast<uint32_t>(tex->Height));
                tex->SetStatus(ImTextureStatus_OK);
            }
            else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
            {
                imgui.destroyTexture(static_cast<uint64_t>(tex->GetTexID()));
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    inline NriImguiDrawDataDesc translateImGuiNoxRHIDrawData(ImDrawData* drawData, uint32_t frameSlot, uint32_t viewportSlot, ImGuiNoxRHIScratch& scratch)
    {
        scratch.drawLists.clear();
        scratch.drawCmds.clear();

        if (!drawData || drawData->CmdListsCount == 0)
            return {};

        size_t totalCmds = 0;
        for (const ImDrawList* list : drawData->CmdLists)
            totalCmds += static_cast<size_t>(list->CmdBuffer.Size);

        scratch.drawLists.reserve(static_cast<size_t>(drawData->CmdListsCount));
        scratch.drawCmds.reserve(totalCmds);

        for (const ImDrawList* list : drawData->CmdLists)
        {
            const size_t cmdStart = scratch.drawCmds.size();
            for (const ImDrawCmd& cmd : list->CmdBuffer)
            {
                if (cmd.UserCallback)
                    continue;
                scratch.drawCmds.push_back(NriImguiDrawCmd{
                    .clipRect = { cmd.ClipRect.x, cmd.ClipRect.y, cmd.ClipRect.z, cmd.ClipRect.w },
                    .elemCount = cmd.ElemCount,
                    .idxOffset = cmd.IdxOffset,
                    .vtxOffset = static_cast<int32_t>(cmd.VtxOffset),
                    .textureId = static_cast<uint64_t>(cmd.GetTexID())
                });
            }

            scratch.drawLists.push_back(NriImguiDrawList{
                .vertices = reinterpret_cast<const NriImguiVertex*>(list->VtxBuffer.Data),
                .vertexCount = static_cast<uint32_t>(list->VtxBuffer.Size),
                .indices = list->IdxBuffer.Data,
                .indexCount = static_cast<uint32_t>(list->IdxBuffer.Size),
                .indexSize = sizeof(ImDrawIdx),
                .cmds = scratch.drawCmds.data() + cmdStart,
                .cmdCount = static_cast<uint32_t>(scratch.drawCmds.size() - cmdStart)
            });
        }

        return NriImguiDrawDataDesc{
            .drawLists = scratch.drawLists.data(),
            .drawListCount = static_cast<uint32_t>(scratch.drawLists.size()),
            .displayPos = { drawData->DisplayPos.x, drawData->DisplayPos.y },
            .displaySize = { drawData->DisplaySize.x, drawData->DisplaySize.y },
            .framebufferScale = { drawData->FramebufferScale.x, drawData->FramebufferScale.y },
            .frameIndex = frameSlot,
            .viewportSlot = viewportSlot
        };
    }

    inline void rendererCreateWindow(ImGuiViewport* viewport)
    {
        auto* bd = getImGuiNoxRHIData();
        if (!bd)
            return;

        NriNativeWindowHandle handle{};
#ifdef _WIN32
        handle.type = NRI_WINDOW_SYSTEM_WIN32;
        handle.hwnd = viewport->PlatformHandleRaw ? viewport->PlatformHandleRaw : viewport->PlatformHandle;
#endif
        const uint64_t vpHandle = bd->imgui.createViewport(NriImguiViewportDesc{
            .window = handle,
            .width = static_cast<uint32_t>(std::max(1.0f, viewport->Size.x)),
            .height = static_cast<uint32_t>(std::max(1.0f, viewport->Size.y)),
            .vsync = false
        });
        viewport->RendererUserData = reinterpret_cast<void*>(static_cast<uintptr_t>(vpHandle));
    }

    inline void rendererDestroyWindow(ImGuiViewport* viewport)
    {
        auto* bd = getImGuiNoxRHIData();
        const uint64_t vpHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(viewport->RendererUserData));
        if (bd && vpHandle != 0)
            bd->imgui.destroyViewport(vpHandle);
        viewport->RendererUserData = nullptr;
    }

    inline void rendererSetWindowSize(ImGuiViewport* viewport, ImVec2 size)
    {
        auto* bd = getImGuiNoxRHIData();
        const uint64_t vpHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(viewport->RendererUserData));
        if (bd && vpHandle != 0)
            bd->imgui.resizeViewport(vpHandle, static_cast<uint32_t>(std::max(0.0f, size.x)), static_cast<uint32_t>(std::max(0.0f, size.y)));
    }

    inline void rendererRenderWindow(ImGuiViewport* viewport, void*)
    {
        auto* bd = getImGuiNoxRHIData();
        const uint64_t vpHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(viewport->RendererUserData));
        if (!bd || vpHandle == 0)
            return;

        syncImGuiNoxRHITextures(bd->imgui, viewport->DrawData);
        const uint32_t vpSlot = bd->imgui.getViewportSlot(vpHandle);
        const NriImguiDrawDataDesc dd = translateImGuiNoxRHIDrawData(viewport->DrawData, 0, vpSlot, bd->viewportScratch);
        const bool noBg = (viewport->Flags & ImGuiViewportFlags_NoRendererClear) != 0;
        bd->imgui.renderViewport(vpHandle, dd, noBg ? nri::ClearColor{ 0.0f, 0.0f, 0.0f, 0.0f } : nri::ClearColor{ 0.0f, 0.0f, 0.0f, 1.0f });
    }

    inline void rendererSwapBuffers(ImGuiViewport* viewport, void*)
    {
        auto* bd = getImGuiNoxRHIData();
        const uint64_t vpHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(viewport->RendererUserData));
        if (bd && vpHandle != 0)
            bd->imgui.presentViewport(vpHandle);
    }
}

inline bool ImGui_ImplNoxRHI_Init(const ImGui_ImplNoxRHI_InitInfo& info)
{
    if (!info.device || !info.device->handle() || !ImGui::GetCurrentContext())
        return false;

    ImGuiIO& io = ImGui::GetIO();
    if (io.BackendRendererUserData != nullptr)
        return false;

    auto* bd = new detail::ImGuiNoxRHIData();
    bd->viewportsEnabled = info.enableViewports;
    bd->imgui = nri::createImgui(*info.device, {
        .resourceHeap = info.resourceHeap,
        .samplerHeap = info.samplerHeap,
        .samplerSlot = info.samplerSlot,
        .colorFormat = info.colorFormat,
        .framesInFlight = info.framesInFlight > 0 ? info.framesInFlight : 2,
        .linearColorOutput = info.linearColorOutput
    });

    io.BackendRendererName = "imgui_impl_noxrhi";
    io.BackendRendererUserData = bd;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    if (info.enableViewports)
    {
        io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
        ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
        platformIO.Renderer_CreateWindow = detail::rendererCreateWindow;
        platformIO.Renderer_DestroyWindow = detail::rendererDestroyWindow;
        platformIO.Renderer_SetWindowSize = detail::rendererSetWindowSize;
        platformIO.Renderer_RenderWindow = detail::rendererRenderWindow;
        platformIO.Renderer_SwapBuffers = detail::rendererSwapBuffers;
    }

    return true;
}

inline void ImGui_ImplNoxRHI_Shutdown()
{
    ImGuiIO& io = ImGui::GetIO();
    auto* bd = detail::getImGuiNoxRHIData();
    if (!bd)
        return;

    if (bd->viewportsEnabled)
        ImGui::DestroyPlatformWindows();

    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures |
                         ImGuiBackendFlags_RendererHasVtxOffset |
                         ImGuiBackendFlags_RendererHasViewports);
    delete bd;
}

inline void ImGui_ImplNoxRHI_NewFrame()
{
    // Font/dynamic atlas uploads are driven cleanly by ImDrawData::Textures in PrepareDrawData.
}

inline void ImGui_ImplNoxRHI_SyncTextures(ImDrawData* drawData)
{
    if (auto* bd = detail::getImGuiNoxRHIData())
        detail::syncImGuiNoxRHITextures(bd->imgui, drawData);
}

// Syncs ImGui textures and translates ImDrawData into an NRI draw descriptor backed by internal scratch storage.
inline NriImguiDrawDataDesc ImGui_ImplNoxRHI_PrepareDrawData(ImDrawData* drawData, uint32_t frameSlot, uint32_t viewportSlot = 0)
{
    auto* bd = detail::getImGuiNoxRHIData();
    if (!bd)
        return {};

    detail::syncImGuiNoxRHITextures(bd->imgui, drawData);
    return detail::translateImGuiNoxRHIDrawData(drawData, frameSlot, viewportSlot, bd->mainScratch);
}

// Uploads vertex/index geometry before beginning the render pass (e.g., in an upload RenderGraph pass).
inline void ImGui_ImplNoxRHI_CopyData(nri::CommandBuffer& cmd, const NriImguiDrawDataDesc& drawData)
{
    if (auto* bd = detail::getImGuiNoxRHIData())
        bd->imgui.copyData(cmd, drawData);
}

// Draws previously uploaded ImGui geometry inside an active raster render pass.
inline void ImGui_ImplNoxRHI_RenderDrawData(nri::CommandBuffer& cmd, const NriImguiDrawDataDesc& drawData)
{
    if (auto* bd = detail::getImGuiNoxRHIData())
        bd->imgui.draw(cmd, drawData);
}

inline nri::Imgui* ImGui_ImplNoxRHI_GetImgui()
{
    auto* bd = detail::getImGuiNoxRHIData();
    return bd ? &bd->imgui : nullptr;
}
