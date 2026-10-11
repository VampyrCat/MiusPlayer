import ghidra.GhidraApplicationLayout;
import ghidra.GhidraLaunchable;
import ghidra.framework.Application;
import ghidra.framework.HeadlessGhidraApplicationConfiguration;
import ghidra.base.project.GhidraProject;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.util.task.TaskMonitor;
import java.io.*;
import java.util.*;
import java.util.regex.*;

public class GtDump implements GhidraLaunchable {
    DecompInterface dec; PrintWriter w; Set<Address> done = new HashSet<>(); Program p;
    void decomp(Function f, String why) {
        if (f == null || !done.add(f.getEntryPoint())) return;
        DecompileResults r = dec.decompileFunction(f, 120, TaskMonitor.DUMMY);
        w.println("\n//==== " + f.getName() + " @ " + f.getEntryPoint() + "  (" + why + ")");
        if (r != null && r.getDecompiledFunction() != null) w.println(r.getDecompiledFunction().getC());
        else w.println("// decompile failed");
    }
    public static void main(String[] a) throws Exception { new GtDump().launch(new GhidraApplicationLayout(new File("G:/ghidra_12.1.4_PUBLIC")), a); }
    public void launch(GhidraApplicationLayout layout, String[] a) throws Exception {
        Application.initializeApplication(layout, new HeadlessGhidraApplicationConfiguration());
        File projDir = new File(a[0]); String outDir = a[1];
        Pattern pat = Pattern.compile(a[2].replace("@@", "|"));
        int max = Integer.parseInt(a[3]);
        GhidraProject gp;
        try { gp = GhidraProject.openProject(projDir.getPath(), "gt", false); }
        catch (Exception e) { gp = GhidraProject.createProject(projDir.getPath(), "gt", false); }
        for (int i = 4; i < a.length; i++) {
            File f = new File(a[i]);
            if (a[i].startsWith("fn:") || a[i].startsWith("vt:")) continue;
            try { p = gp.openProgram("/", f.getName(), false); if (a[2].equals("-")) { } }
            catch (Exception e) {
                System.out.println("import+analyze " + f.getName());
                p = gp.importProgram(f);
                gp.analyze(p);
                gp.saveAs(p, "/", f.getName(), true);
            }
            done.clear();
            w = new PrintWriter(new FileWriter(new File(outDir, f.getName() + ".txt")));
            dec = new DecompInterface(); dec.openProgram(p);
            FunctionManager fm = p.getFunctionManager();
            w.println("// RTTI class names");
            List<Data> hits = new ArrayList<>();
            DataIterator it = p.getListing().getDefinedData(true);
            while (it.hasNext()) {
                Data d = it.next();
                if (!d.hasStringValue()) continue;
                String s = String.valueOf(d.getValue());
                if (s.startsWith(".?A")) w.println("//   " + s);
                if (pat.matcher(s).find()) hits.add(d);
            }
            AddressIterator eps = p.getSymbolTable().getExternalEntryPointIterator();
            while (eps.hasNext()) {
                Function fn = fm.getFunctionAt(eps.next());
                if (fn == null || fn.getName().equals("entry")) continue;
                decomp(fn, "export");
                for (Function c : fn.getCalledFunctions(TaskMonitor.DUMMY)) if (!c.isThunk() && !c.isExternal()) decomp(c, "called by " + fn.getName());
            }
            for (int j = 4; j < a.length; j++) if (a[j].startsWith("fn:")) {
                String[] q = a[j].substring(3).split("=");
                if (q[0].equals(f.getName())) decomp(fm.getFunctionContaining(p.getAddressFactory().getAddress(q[1])), "requested");
            }
            for (int j = 4; j < a.length; j++) if (a[j].startsWith("vt:")) {
                String[] q = a[j].substring(3).split("=");
                if (!q[0].equals(f.getName())) continue;
                String[] r = q[1].split(",");
                Address va = p.getAddressFactory().getAddress(r[0]);
                w.println("\n// VTABLE @ " + va);
                for (int k = 0; k < Integer.parseInt(r[1]); k++) {
                    long t = p.getMemory().getLong(va.add(8L * k));
                    Address ta = va.getNewAddress(t);
                    Function fn = fm.getFunctionAt(ta);
                    if (fn == null && p.getMemory().contains(ta) && p.getMemory().getBlock(ta).isExecute()) {
                        int tx = p.startTransaction("mk");
                        new ghidra.app.cmd.disassemble.DisassembleCommand(ta, null, true).applyTo(p);
                        new ghidra.app.cmd.function.CreateFunctionCmd(ta).applyTo(p);
                        p.endTransaction(tx, true);
                        fn = fm.getFunctionAt(ta);
                    }
                    w.println("//   [" + k + "] +0x" + Integer.toHexString(8 * k) + " -> " + ta + " " + (fn == null ? "(no func)" : fn.getName()));
                    if (fn != null) decomp(fn, "vtable " + va + " slot " + k);
                }
            }
            int n = 0;
            for (Data d : hits) {
                w.println("\n// STRING '" + d.getValue() + "' @ " + d.getAddress());
                ReferenceIterator ri = p.getReferenceManager().getReferencesTo(d.getAddress());
                while (ri.hasNext()) {
                    Reference r = ri.next();
                    Function fn = fm.getFunctionContaining(r.getFromAddress());
                    w.println("//   ref from " + r.getFromAddress() + " in " + (fn == null ? "?" : fn.getName()));
                    if (fn != null && n < max && !done.contains(fn.getEntryPoint())) { n++; decomp(fn, "refs '" + d.getValue() + "'"); }
                }
            }
            w.close(); dec.dispose(); gp.close(p);
            System.out.println("done " + f.getName());
        }
        gp.close();
        System.exit(0);
    }
}
