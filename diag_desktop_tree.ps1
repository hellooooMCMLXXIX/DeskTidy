# 诊断：输出桌面窗口层级（Progman/WorkerW 与 SHELLDLL_DefView 的关系）
# 用于决定"CDeskTidyWidget 小窗口以哪个桌面窗口为 Owner"（WitchDrawer 方案）
$src = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class T
{
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int idx);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
}
'@
Add-Type -TypeDefinition $src

$tops = New-Object System.Collections.ArrayList
$cb = [T+EnumProc]{ param($h,$lp) $tops.Add($h) | Out-Null; $true }
[T]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
$sh = [T]::GetShellWindow()
Write-Output ("GetShellWindow=0x{0:X}" -f $sh.ToInt64())
foreach ($t in $tops)
{
    $sb = New-Object System.Text.StringBuilder 64
    [T]::GetClassName($t, $sb, 64) | Out-Null
    $cls = $sb.ToString()
    if ($cls -ne 'Progman' -and $cls -ne 'WorkerW') { continue }
    $ex = [T]::GetWindowLong($t, -20)
    $vis = [T]::IsWindowVisible($t)
    Write-Output ("TOP [{0}] 0x{1:X} ex=0x{2:X} vis={3} shell={4}" -f $cls, $t.ToInt64(), $ex, $vis, ($t -eq $sh))
    $kids = New-Object System.Collections.ArrayList
    $cb2 = [T+EnumProc]{ param($h,$lp) $kids.Add($h) | Out-Null; $true }
    [T]::EnumChildWindows($t, $cb2, [IntPtr]::Zero) | Out-Null
    foreach ($k in $kids)
    {
        $sb2 = New-Object System.Text.StringBuilder 64
        [T]::GetClassName($k, $sb2, 64) | Out-Null
        $exk = [T]::GetWindowLong($k, -20)
        Write-Output ("    CHILD [{0}] 0x{1:X} ex=0x{2:X}" -f $sb2.ToString(), $k.ToInt64(), $exk)
    }
}
