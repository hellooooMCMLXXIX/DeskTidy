Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Shot {
    [DllImport("user32.dll")] public static extern IntPtr GetDesktopWindow();
    [DllImport("user32.dll")] public static extern IntPtr GetWindowDC(IntPtr hWnd);
    [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr hdcDest, int x, int y, int w, int h, IntPtr hdcSrc, int x1, int y1, int copyOp);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int nIndex);
    [DllImport("user32.dll")] public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public KEYBDINPUT ki; }
    public static void PressWinD() {
        INPUT[] inp = new INPUT[4];
        inp[0].type = 1; inp[0].ki.wVk = 0x5B;
        inp[1].type = 1; inp[1].ki.wVk = 0x44;
        inp[2].type = 1; inp[2].ki.wVk = 0x44; inp[2].ki.dwFlags = 2;
        inp[3].type = 1; inp[3].ki.wVk = 0x5B; inp[3].ki.dwFlags = 2;
        SendInput(4, inp, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@

function Capture-Screen([string]$Path) {
    $w = [Shot]::GetSystemMetrics(0)
    $h = [Shot]::GetSystemMetrics(1)
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $src = [Shot]::GetWindowDC([Shot]::GetDesktopWindow())
    [Shot]::BitBlt($hdc, 0, 0, $w, $h, $src, 0, 0, 0x00CC0020) | Out-Null
    $g.ReleaseHdc($hdc)
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose()
    $bmp.Dispose()
}

Capture-Screen "d:\WorkSpace\Workspace-CPP\DeskTidy\shot_normal.png"
[Shot]::PressWinD()
Start-Sleep -Milliseconds 2000
Capture-Screen "d:\WorkSpace\Workspace-CPP\DeskTidy\shot_desktop.png"
[Shot]::PressWinD()
Start-Sleep -Milliseconds 2000
