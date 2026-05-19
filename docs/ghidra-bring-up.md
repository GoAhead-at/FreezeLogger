# Ghidra Bring-Up — Hook-Target Reference

> **Status for 1.5.97:** The two heartbeat hook targets are **already pinned**
> in the source tree (`src/MainHook.cpp`, `src/RenderHook.cpp`):
>
> - `RE::Main::Update` CALL site → `REL::ID(35551)` + `0x11F`
> - `BSGraphics::Renderer::Init_InitD3D` CALL site → `REL::ID(75595)` + `0x50`
>   (gateway to `IDXGISwapChain::Present` vtable slot 8 — no REL::ID for
>   Present itself; we use the DXGI vtable directly.)
>
> These were cross-verified against two independent commits of
> `doodlum/skyrim-community-shaders` (`08286310`, `783f5024`); inline citations
> are in the source files.
>
> **You only need this document if** (a) you're adding a new runtime (AE/VR),
> (b) one of the IDs ever fails to resolve at load time, or (c) you want to
> verify the pinned values against your local binary as a paranoia check.

**Goal of the workflow below:** find the function addresses for
`RE::Main::Update` and the call site we hook for `Init_InitD3D` (or
`BSGraphics::Renderer::Present` if you'd rather hook the wrapper directly)
in `SkyrimSE.exe.unpacked.exe`, then plug them into the hook code.

This document covers only what's needed for *this* plugin. It is not a
general Ghidra tutorial.

> All paths below match the dev machine layout from `docs/spec.md` §9.1:
> - Ghidra: `D:\Programme\ghidra_12.0.4`
> - Unpacked SkyrimSE: `D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe`
> - Address Library bin: `D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\Data\SKSE\Plugins\version-1-5-97-0.bin` (or wherever Address Library was installed by MO2)

---

## 1. One-time project setup

1. Launch `D:\Programme\ghidra_12.0.4\ghidraRun.bat`.
2. **File → New Project…** → *Non-Shared Project* → name it `Skyrim_1_5_97`,
   pick a workspace dir.
3. **File → Import File…** → select
   `D:\SPIELE\nolvus\Instances\Nolvus Awakening\STOCK GAME\SkyrimSE.exe.unpacked.exe`.
4. Accept the detected format (`Portable Executable (PE)`, x86-64). Click *OK*.
5. Double-click the imported file to open it in CodeBrowser. When prompted to
   *Analyze*, click **Yes**, accept the default analyzers (leave
   *RTTI Analyzer*, *Decompiler Parameter ID*, *PDB Universal* on).
6. Auto-analysis on Skyrim takes 5–15 minutes. **Save the project when it
   finishes** — every later session can re-open it instantly.

You only do steps 1–6 once. Re-open the project for every subsequent session.

---

## 2. Find `BSGraphics::Renderer::Present` (do this first — it's easier)

The renderer's per-frame `Present` is a thin wrapper around
`IDXGISwapChain::Present`. Skyrim links DXGI dynamically, so the import is
visible as a named symbol in Ghidra.

### Walkthrough

1. **Window → Symbol Tree**.
2. In the tree, expand `Imports → DXGI.dll`.
3. Find an entry named `IDXGISwapChain::Present` (Ghidra usually labels it
   exactly that; if not, look for `Present` as the bottom of a vtable list).
4. Right-click the import → **References → Show References To Address**.
   You'll get a list of every call site that ultimately reaches Present.
5. Most of those call sites are in **one Skyrim function** — that function
   is `BSGraphics::Renderer::Present`. The other call sites are typically
   the renderer's vtable initialization or error paths.
6. Double-click each candidate caller. The right one:
   - is small (≈10–40 lines decompiled)
   - takes 3 parameters (`this`, `syncInterval`, `flags`) or similar
   - calls `IDXGISwapChain::Present` exactly once near the top
   - does not call any Skyrim-specific RE classes from above the Present
7. **Note the function's start address.** Example: `141234567` would be the
   absolute VA. Subtract the image base (Ghidra shows it in the lower-left
   status bar — for Skyrim 1.5.97 it's `0x140000000`) to get the **RVA**:
   `141234567 - 140000000 = 0x01234567`.

> **Tip — automated**: see the `find_present_callers.py` script in
> `tools/ghidra/`. Run it from Ghidra's *Window → Script Manager* and it
> prints all callers of `IDXGISwapChain::Present` with their RVAs.

### Sanity-check

The Present wrapper, decompiled, should look roughly like:

```c
void BSGraphics_Renderer_Present(void *this, uint syncInterval, uint flags) {
    // ... usually a brief setup block ...
    (*pSwapChain->lpVtbl->Present)(pSwapChain, syncInterval, flags);
    // ... maybe some post-frame bookkeeping ...
}
```

If the candidate matches that shape — that's it. Record the RVA.

---

## 3. Find `RE::Main::Update`

`RE::Main` is the engine's per-process root object. `Update` is its per-frame
tick. There are three reliable strategies; try them in order.

### Strategy A — RTTI symbol search

1. **Window → Symbol Table** (not Symbol Tree).
2. Filter by *Name* containing `Main`.
3. Look for an entry like `class Main` or a vtable named `Main::__vftable`.
4. If RTTI was demangled, you'll see `Main::Update` directly. Done — record
   its RVA.

If Skyrim's binary doesn't expose RTTI for `Main` (often the case for
singleton classes without virtual functions), this strategy fails — go to B.

### Strategy B — Trace from `WinMain`

1. **Window → Symbol Table** → filter for `WinMain` or `wWinMain`.
2. Double-click to jump to it.
3. Decompile (`F5` if not already in the decompiler view). The function
   structure is roughly:
   ```
   WinMain:
       initialize_*();
       run_main_loop();   // ← this is what we want
       cleanup_*();
       return ...;
   ```
4. Step into `run_main_loop`. Inside, you'll see a `while` / `for` body that
   pumps Windows messages and calls into the engine each iteration. The
   biggest call inside that body — typically the one that takes a `float`
   delta-time parameter — is **`RE::Main::Update`**.
5. Right-click that call → *Go to function*. Record its RVA.

### Strategy C — Search for the per-frame `delta` signature

1. **Search → For Instructions → For Bytes…** *(or Decompiler Parameter ID
   if the previous analysis pass populated it)*.
2. In *Defined Strings*, filter for unique per-frame log strings like
   `"FPS"`, `"frametime"`, or known engine debug strings — these often
   appear inside or near `Main::Update`.
3. Cross-reference uses of those strings to functions; the one called every
   frame from the message pump is `Main::Update`.

Strategy A and B are usually enough; C is the fallback.

### Sanity-check

`Main::Update`, decompiled, should:

- take **two parameters** (`Main* this` and `float deltaTime`),
- call into many subsystems (input, AI, animation, Havok, render submission)
  in a recognisable per-frame ordering,
- not loop indefinitely (no `while(true)` covering the whole body).

---

## 4. Plugging the result back into the plugin

You now have two RVAs. There are two ways to use them.

### Option 1 — by Address Library ID *(preferred)*

Look up the RVA in `version-1-5-97-0.bin` to find its ID. Easiest path:
search any open-source CommonLibSSE-NG plugin on GitHub that already hooks
the same function on 1.5.97 (`Main::Update` is hooked by dozens of plugins;
e.g. EngineFixes, ENB Helper, ConsolePlusPlus, SmoothCam — pick one and
read off its `RELOCATION_ID(SE, AE)` constant). Use the SE half.

Then in `src/MainHook.cpp`:

```cpp
constexpr std::uint64_t kMainUpdateID = <the_id>;  // pinned from <source>
```

Verification: at install time the plugin logs `MainHook installed (REL::ID
N)` along with the resolved address. Compare that address against the RVA
you measured in Ghidra. If they match, you've got it.

### Option 2 — by direct RVA *(works without Address Library at all)*

CommonLibSSE-NG accepts a raw offset. Replace the `REL::ID(...)` site with
`REL::Offset(rva)`:

```cpp
// In src/MainHook.cpp
constexpr std::uintptr_t kMainUpdateRVA = 0x01234567;  // from Ghidra
REL::Relocation<std::uintptr_t> target{ REL::Offset(kMainUpdateRVA) };
```

Trade-off: bypasses Address Library. Our existing runtime-version pin
(spec §0, refuses to load on anything ≠ 1.5.97) is what protects you here.
This is fine for a personal-first build, but if you later release publicly
**switch to Option 1** so users on slightly different patch builds get a
loud refusal instead of a hooked-wrong-address corruption.

---

## 5. Verifying the hooks

After building the **debug** preset:

1. Launch Skyrim through MO2 with the debug DLL installed.
2. Check `Documents\My Games\Skyrim Special Edition\SKSE\FreezeLogger.log`.
   Three lines must appear (in this order):
   ```
   MainHook installed at 0x… (Main::Update CALL site, REL::ID 35551 +0x11F).
   RenderHook: armed Init_InitD3D hook (REL::ID 75595 +0x50); swap-chain detour will install when D3D11 is created.
   RenderHook: detoured IDXGISwapChain::Present (vtable slot 8) at 0x…
   ```
   The third line appears slightly later than the first two — once Skyrim
   has actually created its D3D11 device and swap chain.
3. Press `Pause/Break`. Within ~5.5 s a freeze report must appear in
   `…\SKSE\FreezeLogger\freeze_<ts>_main.log`.
4. Open the report. The threads section should contain **a stack ending in
   `Sleep`** for the main game thread — that's our deliberate stall, proving
   the heartbeat → watchdog → snapshot path is wired end to end.

If step 3 produces no file, check the plugin log for an
`MainHook installed`/`RenderHook: armed` line — if either is missing, the
hook failed to install (probably an Address Library / runtime mismatch). If
step 4 shows `<unavailable: caught SEH …>` for a section, that section's
RE accessors need a `TODO_RE` pass.

---

## 6. Refining the hook style after Ghidra reveals the call shape

The current `MainHook.cpp` and `RenderHook.cpp` use
`trampoline.write_call<5>(target.address(), HookedFn)`, which assumes
**`target.address()` is the start of a `CALL` instruction we want to
intercept**. After looking at the function in Ghidra, you may discover one
of these patterns is more appropriate:

| Pattern | When to use it | CommonLibSSE-NG helper |
|---|---|---|
| `write_call<5>` over a specific call site | A single call-site to the target, e.g. `WinMain → Main::Update` | `SKSE::GetTrampoline().write_call<5>(callSiteAddr, &Hook)` |
| Detour at the function entry | We want to intercept *every* invocation of the function | `SKSE::GetTrampoline().write_branch<5>(funcAddr, &Hook)` |
| vtable replacement | The function is virtual on a known class | `stl::write_vfunc<RE::Class, HookStruct>()` |

For `Main::Update` the canonical idiom in NG plugins is **`write_call<5>` at
the call site inside `WinMain`'s main loop**, not a detour at Update's
entry. If Ghidra shows that, tweak `kMainUpdateID` to be the *containing
function's* ID and add an offset:

```cpp
REL::Relocation<std::uintptr_t> hook{ REL::ID(kContainingID), 0xNNN /*offset to CALL*/ };
SKSE::GetTrampoline().write_call<5>(hook.address(), HookedUpdate);
```

For `Renderer::Present` either entry-detour or a single call-site call works
in practice; pick whichever Ghidra makes obvious.

---

## 7. Ghidra Python helper

`tools/ghidra/find_present_callers.py` automates the Renderer::Present
search from §2. Run it via Ghidra's *Window → Script Manager* (you'll be
asked to register the script directory the first time).
