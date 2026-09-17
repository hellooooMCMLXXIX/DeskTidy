using System;
using System.Runtime.InteropServices;
using System.Text;

public class DiagTree
{
    public delegate bool EnumProc(IntPtr h, IntPtr lp);

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int idx);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);

    public static void Main()
    {
        System.Collections.ArrayList tops = new System.Collections.ArrayList();
        EnumWindows(delegate(IntPtr h, IntPtr lp) { tops.Add(h); return true; }, IntPtr.Zero);
        IntPtr sh = GetShellWindow();
        Console.WriteLine("GetShellWindow=0x" + sh.ToInt64().ToString("X"));
        foreach (IntPtr t in tops)
        {
            StringBuilder sb = new StringBuilder(64);
            GetClassName(t, sb, 64);
            string cls = sb.ToString();
            if (cls != "Progman" && cls != "WorkerW") continue;
            int ex = GetWindowLong(t, -20);
            bool vis = IsWindowVisible(t);
            Console.WriteLine("TOP [{0}] 0x{1:X} ex=0x{2:X} vis={3} shell={4}",
                cls, t.ToInt64(), ex, vis, t == sh);
            System.Collections.ArrayList kids = new System.Collections.ArrayList();
            EnumChildWindows(t, delegate(IntPtr h, IntPtr lp) { kids.Add(h); return true; }, IntPtr.Zero);
            foreach (IntPtr k in kids)
            {
                StringBuilder sb2 = new StringBuilder(64);
                GetClassName(k, sb2, 64);
                int exk = GetWindowLong(k, -20);
                Console.WriteLine("    CHILD [{0}] 0x{1:X} ex=0x{2:X}",
                    sb2.ToString(), k.ToInt64(), exk);
            }
        }
    }
}
