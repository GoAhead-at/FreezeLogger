# Ghidra headless script: resolve a list of RVAs to their containing
# functions and dump disassembly + a short decompiled body around each
# offset. Designed to be run from analyzeHeadless.bat with a binary
# already imported and analyzed, e.g.:
#
#   analyzeHeadless.bat <project_dir> <project_name> ^
#     -process hdtsmp64.dll ^
#     -postScript resolve_offsets.py 0x42dfe 0x176 0x...
#
# If the project does not yet exist, the caller should -import the binary
# in a first invocation (so analysis runs once), then re-run with
# -process to reuse the analyzed program.
#
# Each argument is a hex RVA (with or without the leading 0x). The script
# prints, for each:
#   - the containing function name + entry RVA + size
#   - the instruction at the given RVA
#   - the function's full disassembly (truncated)
#   - the function's decompilation (truncated)
#
# Compatible with Ghidra 11.x / 12.x. Uses Jython 2.7 (Ghidra's bundled).
#
# @author      FreezeLogger
# @category    FreezeLogger
# @runtime     Jython

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


MAX_DISASM_LINES = 80
MAX_DECOMP_LINES = 120


def parse_rva(s):
    s = s.strip()
    if s.lower().startswith("0x"):
        s = s[2:]
    return int(s, 16)


def fmt_rva(prog, addr):
    return "0x{:x}".format(addr.getOffset() - prog.getImageBase().getOffset())


def function_at(prog, rva):
    addr = prog.getImageBase().add(rva)
    fm = prog.getFunctionManager()
    f = fm.getFunctionContaining(addr)
    return addr, f


def disasm_lines(prog, func, target_addr):
    listing = prog.getListing()
    body = func.getBody()
    iter_ = listing.getInstructions(body, True)
    out = []
    for ins in iter_:
        marker = " >>> " if ins.getAddress() == target_addr else "     "
        out.append("{}{} {} {}".format(
            marker,
            fmt_rva(prog, ins.getAddress()),
            ins.getMnemonicString().ljust(7),
            ins.toString().split(None, 1)[1] if " " in ins.toString() else "",
        ))
        if len(out) >= MAX_DISASM_LINES:
            out.append("     ... (truncated, function continues)")
            break
    return out


def decompile(prog, func, decompiler):
    res = decompiler.decompileFunction(func, 30, ConsoleTaskMonitor())
    if not res or not res.getDecompiledFunction():
        return ["    <decompilation failed>"]
    src = res.getDecompiledFunction().getC()
    lines = src.split("\n")
    if len(lines) > MAX_DECOMP_LINES:
        lines = lines[:MAX_DECOMP_LINES]
        lines.append("    /* ... (truncated) ... */")
    return lines


def main():
    prog = currentProgram   # noqa: F821 — provided by Ghidra
    args = list(getScriptArgs())   # noqa: F821 — provided by Ghidra

    print("=" * 78)
    print("resolve_offsets.py")
    print("Program:    {}".format(prog.getName()))
    print("Image base: {}".format(prog.getImageBase()))
    print("Args:       {}".format(args))
    print("=" * 78)

    if not args:
        print("ERROR: No RVAs provided. Pass hex RVAs as -postScript args.")
        return

    decompiler = DecompInterface()
    decompiler.openProgram(prog)

    for raw in args:
        try:
            rva = parse_rva(raw)
        except ValueError:
            print("\n!! could not parse RVA {!r}; skipping".format(raw))
            continue

        addr, func = function_at(prog, rva)
        print("\n" + "-" * 78)
        print("Target RVA   : 0x{:x}  (absolute {})".format(rva, addr))
        if func is None:
            print("Containing fn: <none — address is outside any function>")
            continue
        body = func.getBody()
        size = body.getNumAddresses() if body else 0
        print("Containing fn: {}".format(func.getName(True)))
        print("              entry RVA = {}, size = {} bytes".format(
            fmt_rva(prog, func.getEntryPoint()), size))
        print("              signature = {}".format(func.getSignature()))

        print("\n  Disassembly (target marked '>>>'):")
        for line in disasm_lines(prog, func, addr):
            print("  {}".format(line))

        print("\n  Decompilation:")
        for line in decompile(prog, func, decompiler):
            print("    {}".format(line))

    decompiler.dispose()
    print("\n" + "=" * 78)
    print("done")


main()
