/* SPDX-License-Identifier: GPL-2.0-only */
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.nio.file.*;
import java.io.*;
// Extract the function graph and decompiler output from a mapped ROM 1 stock image.
public class ExportBtRom1 extends GhidraScript {
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length != 2 || !(arguments[0].equals("usb") || arguments[0].equals("uart"))) {
            throw new IllegalArgumentException("expected usb|uart followed by the analysis output directory");
        }
        String transport = arguments[0];
        Path dir = Paths.get(arguments[1], transport);
        Files.createDirectories(dir);
        Listing listing = currentProgram.getListing();
        Address start = toAddr(0x8010a000L);
        Address end = toAddr(transport.equals("uart") ? 0x8011303fL : 0x80112863L);
        AddressSet patch = new AddressSet(start, end);
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        int count = 0;
        int good = 0;
        try (PrintWriter index = new PrintWriter(Files.newBufferedWriter(dir.resolve("functions.tsv")));
             PrintWriter code = new PrintWriter(Files.newBufferedWriter(dir.resolve("decompiled.c")));
             PrintWriter asm = new PrintWriter(Files.newBufferedWriter(dir.resolve("listing.tsv")))) {
            for (Data data : listing.getDefinedData(patch, true)) {
                asm.printf("%s\t%d\tdata\t%s\n", data.getAddress(), data.getLength(), data.getDataType().getName());
            }
            for (Instruction ins : listing.getInstructions(patch, true)) {
                StringBuilder edges = new StringBuilder();
                for (Reference ref : ins.getReferencesFrom()) {
                    if (edges.length() != 0) edges.append(",");
                    edges.append(ref.getReferenceType()).append(":").append(ref.getToAddress());
                }
                asm.printf("%s\t%d\tinstruction\t%s\t%s\n", ins.getAddress(), ins.getLength(), ins.toString(), edges);
            }
            for (Function fn : currentProgram.getFunctionManager().getFunctions(patch, true)) {
                monitor.checkCancelled();
                count++;
                DecompileResults result = decomp.decompileFunction(fn, 15, monitor);
                boolean ok = result.decompileCompleted() && result.getDecompiledFunction() != null;
                index.printf("%s\t%s\t%s\t%d\t%s\n", fn.getEntryPoint(), fn.getName(), fn.getBody(), fn.getBody().getNumAddresses(), ok ? "ok" : result.getErrorMessage().replace('\n', ' '));
                if (ok) {
                    good++;
                    code.println("/* " + fn.getEntryPoint() + " " + fn.getName() + " */");
                    code.println(result.getDecompiledFunction().getC());
                }
            }
            if (index.checkError() || code.checkError() || asm.checkError()) {
                throw new IOException("could not write the decompilation export");
            }
        } finally {
            decomp.dispose();
        }
        println("Functions " + count + ", decompiled " + good + ", output " + dir);
    }
}

