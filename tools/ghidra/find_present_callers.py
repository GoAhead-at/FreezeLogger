# Ghidra script: list every function in SkyrimSE that calls
# IDXGISwapChain::Present, with absolute address and RVA. The Skyrim
# wrapper BSGraphics::Renderer::Present will be one of the printed callers
# (typically the smallest one with exactly 1 Present call).
#
# Usage:
#   1. In Ghidra, open the analyzed SkyrimSE.exe.unpacked.exe.
#   2. Window -> Script Manager -> Manage Script Directories ->
#      add this folder (tools/ghidra) -> close.
#   3. Find "find_present_callers.py" in the script list and run it.
#   4. The Console (Window -> Console) prints the candidates.
#
# Compatible with Ghidra 11.x / 12.x. Uses Jython 2.7 (Ghidra's bundled).
#
# @author      FreezeLogger
# @category    SkyrimSE
# @runtime     Jython

from ghidra.program.model.symbol import RefType


def find_present_imports(prog):
    """Return Symbol objects for any imported function whose name contains
    'Present' and whose namespace mentions DXGI / D3D11."""
    results = []
    sym_table = prog.getSymbolTable()
    for sym in sym_table.getExternalSymbols():
        name = sym.getName()
        ns = sym.getParentNamespace().getName(True) if sym.getParentNamespace() else ""
        if "Present" in name and ("dxgi" in ns.lower() or "d3d11" in ns.lower() or "DXGI" in ns):
            results.append(sym)
    if results:
        return results

    # Fallback: any external symbol literally named Present.
    for sym in sym_table.getExternalSymbols():
        if sym.getName() == "Present":
            results.append(sym)
    return results


def callers_of(prog, addr):
    """Return the set of Function objects that contain a CALL/REF to addr."""
    fm = prog.getFunctionManager()
    refs = prog.getReferenceManager().getReferencesTo(addr)
    funcs = set()
    for ref in refs:
        if not (ref.getReferenceType().isCall() or ref.getReferenceType() == RefType.UNCONDITIONAL_CALL):
            # Be lenient — some Skyrim call sites end up as DATA refs through
            # function pointers. Keep them too.
            pass
        f = fm.getFunctionContaining(ref.getFromAddress())
        if f is not None:
            funcs.add(f)
    return funcs


def func_summary(func):
    body = func.getBody()
    size = body.getNumAddresses() if body else 0
    return "addr={addr} rva={rva} size={size}b name={name}".format(
        addr=func.getEntryPoint(),
        rva=func.getEntryPoint().getOffset() - func.getProgram().getImageBase().getOffset(),
        size=size,
        name=func.getName(True),
    )


def fmt_rva(prog, addr):
    return "0x{:x}".format(addr.getOffset() - prog.getImageBase().getOffset())


def main():
    prog = currentProgram   # noqa: F821 — provided by Ghidra
    print("=" * 72)
    print("FreezeLogger / find_present_callers.py")
    print("Image base: {}".format(prog.getImageBase()))
    print("=" * 72)

    imports = find_present_imports(prog)
    if not imports:
        print("No DXGI/D3D11 'Present' import was found. Check Symbol Tree manually.")
        return

    for imp in imports:
        addr = imp.getAddress()
        print("\nImport: {}  at {}  rva={}".format(
            imp.getName(True), addr, fmt_rva(prog, addr)))

        callers = callers_of(prog, addr)
        if not callers:
            print("  (no callers found; check that auto-analysis completed)")
            continue

        ranked = sorted(callers, key=lambda f: f.getBody().getNumAddresses() if f.getBody() else 0)
        print("  {} caller function(s) (smallest first):".format(len(ranked)))
        for f in ranked:
            print("    {}".format(func_summary(f)))

    print("\nPick the smallest caller that, when decompiled, calls the import")
    print("exactly once near the top. That is BSGraphics::Renderer::Present.")
    print("Record its RVA and plug into src/RenderHook.cpp (see docs/ghidra-bring-up.md §4).")


main()
