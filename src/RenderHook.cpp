#include "PCH.h"
#include "RenderHook.h"

#include "Heartbeat.h"

#include "RE/B/BSRenderManager.h"

#include <dxgi.h>

namespace FreezeLogger::RenderHook {

    namespace {

        // ---------------------------------------------------------------------
        // Render heartbeat (Skyrim SE 1.5.97).
        //
        // We do NOT hook BSGraphics::Renderer::Present (an internal Skyrim
        // wrapper). Instead we hook IDXGISwapChain::Present directly, by
        // detouring vtable slot 8 of the swap chain object that Skyrim
        // creates. This is the same approach used by every modern NG
        // renderer plugin (community-shaders, ENB Helper, etc.) and has
        // three advantages over hooking the wrapper:
        //
        //   1. No REL::ID needed for Present itself — the DXGI vtable
        //      layout is a stable Microsoft contract.
        //   2. Identical on SE / AE / VR / future runtimes.
        //   3. Catches every Present, regardless of which Skyrim function
        //      ultimately makes the call.
        //
        // Bootstrap sequence:
        //
        //   1. Hook the CALL inside BSGraphics::Renderer::Init_InitD3D
        //      (the function that creates the D3D11 device + swap chain).
        //   2. When that hook fires, run the original (D3D11 init runs,
        //      swap chain materialises in BSRenderManager state).
        //   3. Read the swap-chain pointer via NG's accessor:
        //        RE::BSRenderManager::GetSingleton()->GetRuntimeData().swapChain
        //   4. Detour vtable slot 8 (Present) on that swap chain manually
        //      via VirtualProtect. (NG 3.5.5 has no stl::detour_vfunc
        //      helper, so we do the pointer math ourselves.)
        //
        // Verification of step 1's REL::ID:
        //   Cross-referenced against two independent commits of
        //   doodlum/skyrim-community-shaders (08286310, 783f5024). Both pin
        //   the same site:
        //     stl::write_thunk_call<BSGraphics_Renderer_Init_InitD3D>(
        //       REL::RelocationID(75595, 77226).address()
        //       + REL::Relocate(0x50, 0x2BC));
        //
        // Verification of step 3's accessor path:
        //   build/release/vcpkg_installed/.../include/RE/B/BSRenderManager.h
        //   declares RUNTIME_DATA::swapChain at offset 0x28 inside
        //   BSRenderManager's RUNTIME_DATA block.
        // ---------------------------------------------------------------------
        constexpr std::uint64_t  kInitD3DCallSiteID     = 75595;
        constexpr std::uint64_t  kInitD3DCallSiteOffset = 0x50;
        constexpr std::size_t    kPresentVTableSlot     = 8;

        using InitD3DFn = void(*)();
        using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);

        REL::Relocation<InitD3DFn> g_originalInitD3D;
        PresentFn                  g_originalPresent = nullptr;

        HRESULT WINAPI HookedPresent(IDXGISwapChain* a_this, UINT a_syncInterval, UINT a_flags) {
            Heartbeat::TickRender();
            return g_originalPresent(a_this, a_syncInterval, a_flags);
        }

        void InstallSwapChainDetour() {
            auto* renderManager = RE::BSRenderManager::GetSingleton();
            if (!renderManager) {
                logs::error("RenderHook: BSRenderManager::GetSingleton() returned null after D3D init; "
                            "render heartbeat will not tick.");
                return;
            }

            auto& data = renderManager->GetRuntimeData();
            auto* swapChain = data.swapChain;
            if (!swapChain) {
                logs::error("RenderHook: BSRenderManager has no swap chain after D3D init; "
                            "render heartbeat will not tick.");
                return;
            }

            // IDXGISwapChain has the same vtable layout in every DXGI
            // implementation; slot 8 is Present.
            auto** vtable = *reinterpret_cast<void***>(swapChain);

            DWORD oldProtect = 0;
            if (!::VirtualProtect(&vtable[kPresentVTableSlot],
                                  sizeof(void*),
                                  PAGE_READWRITE,
                                  &oldProtect))
            {
                logs::error("RenderHook: VirtualProtect (RW) failed for swap-chain vtable slot {}; "
                            "GetLastError={}.",
                            kPresentVTableSlot, ::GetLastError());
                return;
            }

            g_originalPresent = reinterpret_cast<PresentFn>(vtable[kPresentVTableSlot]);
            vtable[kPresentVTableSlot] = reinterpret_cast<void*>(&HookedPresent);

            DWORD restoreProtect = 0;
            ::VirtualProtect(&vtable[kPresentVTableSlot],
                             sizeof(void*),
                             oldProtect,
                             &restoreProtect);

            logs::info(
                "RenderHook: detoured IDXGISwapChain::Present (vtable slot {}) at swapChain=0x{:x}.",
                kPresentVTableSlot,
                reinterpret_cast<std::uintptr_t>(swapChain));
        }

        void HookedInitD3D() {
            g_originalInitD3D();
            InstallSwapChainDetour();
        }

    }

    void Install() {
        SKSE::AllocTrampoline(14);
        auto& trampoline = SKSE::GetTrampoline();

        const REL::Relocation<std::uintptr_t> hookSite{
            REL::ID(kInitD3DCallSiteID), kInitD3DCallSiteOffset
        };

        g_originalInitD3D = trampoline.write_call<5>(hookSite.address(), HookedInitD3D);

        logs::info(
            "RenderHook: armed Init_InitD3D hook (REL::ID {} +0x{:x}); "
            "swap-chain detour will install when D3D11 is created.",
            kInitD3DCallSiteID, kInitD3DCallSiteOffset);
    }

}
