// Port of _mp_research\tools\make_progpack.py: the two program definitions the mod needs, made from the
// game's own root\programs.flatdata (read only). The engine prefers a loose root\programs\<name>.progpack
// over the packed one, so these files add multiplayer and deleting them restores the stock game.
//
//   tacgame (battles):   load basic_network.codelib, create the i_network object, call it first in every
//                        simulation step and in the render pipeline; keep simulating when not focused
//   opgame (menus, map): the stock definition already loads and creates it (all known builds); only the
//                        render call is added. Should a build's opgame not list them, they are appended the
//                        same way as for tacgame.
using System;
using System.Collections.Generic;

namespace MiusPlayer
{
    public class GeneratedProgram
    {
        public string Name;          // "tacgame"
        public string RelPath;       // "root/programs/tacgame.progpack"
        public byte[] Data;
    }

    public static class Progpack
    {
        public static readonly string[] Names = { "tacgame", "opgame" };

        public static string RelPathOf(string name)
        {
            return "root/programs/" + name + ".progpack";
        }

        static SortedDictionary<int, string> Col0(string text)
        {
            SortedDictionary<int, string> d = new SortedDictionary<int, string>();
            d[0] = text;
            return d;
        }

        // just below the game's own cursor, so the cursor stays visible on top of the multiplayer menu
        static void AddRenderCall(Azcf cfg)
        {
            List<string> rows = cfg.FirstColumn(cfg.Find("r_main"));
            int at = rows.IndexOf("i_cursor");
            if (at < 0)
                throw new AzcfException("the program's render list r_main has no i_cursor");
            cfg.InsertRow("r_main", at, 0, Col0("i_network"));
        }

        static bool Has(Azcf cfg, string table, string text)
        {
            return cfg.FirstColumn(cfg.Find(table)).IndexOf(text) >= 0;
        }

        // notes: what was done beyond the usual steps (empty for all known builds)
        public static List<GeneratedProgram> Generate(byte[] flatdata, List<string> notes)
        {
            Azcf tac = Azcf.Stock(flatdata, "tacgame");
            tac.AppendStringRow("codelib_list", "basic_network.codelib");
            tac.AppendStringRow("obj_list", "i_network:network");
            tac.InsertRow("p_ctrl", 0, 0, Col0("i_network"));
            AddRenderCall(tac);
            // keep simulating when the window is not focused (alt-tabbed players, two instances on one PC)
            tac.SetField("i_window:window", "lost_fwork", 1);
            tac.SetField("i_loader:loader", "lost_fwork", 1);

            Azcf op = Azcf.Stock(flatdata, "opgame");
            if (!Has(op, "codelib_list", "basic_network.codelib"))
            {
                op.AppendStringRow("codelib_list", "basic_network.codelib");
                if (notes != null) notes.Add("opgame: basic_network.codelib added to codelib_list");
            }
            if (!Has(op, "obj_list", "i_network:network"))
            {
                op.AppendStringRow("obj_list", "i_network:network");
                if (notes != null) notes.Add("opgame: i_network:network added to obj_list");
            }
            AddRenderCall(op);

            List<GeneratedProgram> r = new List<GeneratedProgram>();
            r.Add(Make("tacgame", tac));
            r.Add(Make("opgame", op));
            return r;
        }

        static GeneratedProgram Make(string name, Azcf cfg)
        {
            GeneratedProgram g = new GeneratedProgram();
            g.Name = name;
            g.RelPath = RelPathOf(name);
            g.Data = cfg.Bytes();
            Check(g);
            return g;
        }

        // The written program must read back and have the network object where it belongs.
        static void Check(GeneratedProgram g)
        {
            Azcf c = new Azcf(g.Data);
            if (!Has(c, "codelib_list", "basic_network.codelib") || !Has(c, "obj_list", "i_network:network")
                || !Has(c, "r_main", "i_network") || (g.Name == "tacgame" && !Has(c, "p_ctrl", "i_network")))
                throw new AzcfException("the generated " + g.Name + " program does not read back as expected");
        }
    }
}
