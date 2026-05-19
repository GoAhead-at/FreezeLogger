// @category Analysis
// Headless helper: resolve an RVA (relative to program image base) to a containing symbol.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;

public class FindFunctionAtRva extends GhidraScript {

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args == null || args.length < 1) {
			printerr("Usage: pass RVA as hex, e.g. 0x42dfe or 42dfe");
			return;
		}

		String raw = args[0].trim();
		if (raw.startsWith("0x") || raw.startsWith("0X")) {
			raw = raw.substring(2);
		}
		long rva = Long.parseLong(raw, 16);

		Address base = currentProgram.getImageBase();
		Address target = base.add(rva);
		println("Image base: " + base);
		println("Target RVA: 0x" + Long.toHexString(rva));
		println("Target addr: " + target);

		Function f = getFunctionContaining(target);
		if (f != null) {
			println("Containing function: " + f.getName(true));
			println("  Entry: " + f.getEntryPoint());
			println("  Body:  " + f.getBody());
		} else {
			println("No function containing target (try re-running analysis).");
		}

		Symbol primary = currentProgram.getSymbolTable().getPrimarySymbol(target);
		if (primary != null) {
			println("Primary symbol at target: " + primary.getName(true) + " @ " + primary.getAddress());
		}
	}
}
