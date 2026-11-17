# Boot build\openrom.rom in the test DuckStation (tools\duckstation2, Xplorer cart emulation).
# DuckStation sometimes stalls at start-up when relaunched quickly: retried until the BIOS prints.
# Prints the TTY/debug lines of the log and optionally saves a window capture. Leaves it running.
param([int]$Wait = 6, [string]$Shot = "", [string]$Disc = "")
$d = "$PSScriptRoot\..\..\tools\duckstation2"
function Close-Duck {
    foreach ($q in Get-Process | Where-Object { $_.Path -like "*duckstation2*" }) {
        $q.CloseMainWindow() | Out-Null
        if (-not $q.WaitForExit(10000)) { $q | Stop-Process -Force; Start-Sleep -Seconds 2 }
    }
}
Close-Duck
$rom = [System.IO.File]::ReadAllBytes("$PSScriptRoot\..\build\openrom.rom")
$img = New-Object byte[] 262144
for ($i = 0; $i -lt $img.Length; $i++) { $img[$i] = 0xFF }
[Array]::Copy($rom, $img, $rom.Length)
[System.IO.File]::WriteAllBytes("$d\openrom256.rom", $img)
$lines = (Get-Content "$d\settings.ini") -replace '^FlashImagePath = .*', "FlashImagePath = openrom256.rom"
[System.IO.File]::WriteAllLines("$d\settings.ini", $lines, (New-Object System.Text.UTF8Encoding $false))
for ($try = 0; $try -lt 4; $try++) {
    $p = Start-Process "$d\duckstation-qt-x64-ReleaseLTCG.exe" -ArgumentList $(if ($Disc) { "`"$Disc`"" } else { "-bios" }) -WorkingDirectory $d -PassThru
    $ok = $false
    for ($t = 0; $t -lt 20; $t++) {
        Start-Sleep -Milliseconds 500
        if (Select-String -Path "$d\duckstation.log" -Pattern "I/TTY" -Quiet) { $ok = $true; break }
    }
    if ($ok) { break }
    Close-Duck; Start-Sleep -Seconds 2
}
Start-Sleep -Seconds $Wait
Get-Content "$d\duckstation.log" | Select-String "TTY|SIO" | Select-Object -Last 12 | ForEach-Object { $_.Line }
if ($Shot) { & "$PSScriptRoot\shot.ps1" -ProcId $p.Id -Out $Shot }
"pid $($p.Id) (tries $($try + 1))"
