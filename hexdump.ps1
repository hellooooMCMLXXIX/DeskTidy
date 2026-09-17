$p = 'd:\WorkSpace\Workspace-CPP\DeskTidy\zorder_exstyle_test.ps1'
$b = [System.IO.File]::ReadAllBytes($p)
Write-Output ("filelen=" + $b.Length)
# 找到 "Add-Type @\"" 的位置
$s = [System.Text.Encoding]::UTF8.GetString($b)
$idx = $s.IndexOf('Add-Type')
Write-Output ("Add-Type at " + $idx)
# 输出开头 60 字节的十六进制
$hex = ($b[0..([Math]::Min(60, $b.Length-1))] | ForEach-Object { $_.ToString('X2') }) -join ' '
Write-Output $hex
# 输出 Add-Type 之后的 40 字节
if ($idx -ge 0) {
    $start = $idx
    $end = [Math]::Min($start+40, $b.Length-1)
    $hex2 = ($b[$start..$end] | ForEach-Object { $_.ToString('X2') }) -join ' '
    Write-Output $hex2
}
