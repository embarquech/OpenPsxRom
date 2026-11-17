# Emulator test of the Xplorer switch-off modes: 256 KB image = OpenROM (lower) + original ROM 2
# (upper, from backup\xplorer_before_openrom.bin), settings page OCF1 with -Rom2, switch set by -SwitchOn.
param([switch]$Rom2, [int]$Flag = -1, [switch]$SwitchOn, [int]$Wait = 15)
if ($Rom2 -and $Flag -lt 0) { $Flag = 2 }
$d = "$PSScriptRoot\..\..\tools\duckstation2"
foreach ($q in Get-Process | Where-Object { $_.Path -like "*duckstation2*" }) {
    $q.CloseMainWindow() | Out-Null
    if (-not $q.WaitForExit(10000)) { $q | Stop-Process -Force; Start-Sleep -Seconds 2 }
}
$rom = [IO.File]::ReadAllBytes("$PSScriptRoot\..\build\openrom.rom")
$xp = [IO.File]::ReadAllBytes("$PSScriptRoot\..\backup\xplorer_before_openrom.bin")
$img = New-Object byte[] 262144
[Array]::Copy($rom, 0, $img, 0, 131072)
[Array]::Copy($xp, 131072, $img, 131072, 131072)
for ($i = 0x1FF80; $i -lt 0x20000; $i++) { $img[$i] = 0xFF }
if ($Flag -ge 0) {
    [Array]::Copy([BitConverter]::GetBytes([uint32]0x3146434F), 0, $img, 0x1FF80, 4)
    [Array]::Copy([BitConverter]::GetBytes([uint32]$Flag), 0, $img, 0x1FF84, 4)
}
[IO.File]::WriteAllBytes("$d\openrom256.rom", $img)
$sw = if ($SwitchOn) { "true" } else { "false" }
$lines = (Get-Content "$d\settings.ini") -replace '^FlashImagePath = .*', "FlashImagePath = openrom256.rom" -replace '^SwitchActive = .*', "SwitchActive = $sw"
[IO.File]::WriteAllLines("$d\settings.ini", $lines, (New-Object Text.UTF8Encoding $false))
Remove-Item "$d\duckstation.log" -ErrorAction SilentlyContinue
$p = Start-Process "$d\duckstation-qt-x64-ReleaseLTCG.exe" -ArgumentList "-bios" -WorkingDirectory $d -PassThru
Start-Sleep -Seconds $Wait
Select-String -Path "$d\duckstation.log" -Pattern "I/TTY" | ForEach-Object { $_.Line -replace '^.*I/TTY: ', '' } | Select-Object -First 14
