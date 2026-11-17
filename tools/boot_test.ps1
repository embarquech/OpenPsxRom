# Boot each given flash image in the test DuckStation and report whether the BIOS kernel came up.
param([string[]]$Images, [int]$Wait = 7)
$d = "$PSScriptRoot\..\..\tools\duckstation2"
foreach ($img in $Images) {
    $lines = (Get-Content "$d\settings.ini") -replace '^FlashImagePath = .*', "FlashImagePath = $img"
    [System.IO.File]::WriteAllLines("$d\settings.ini", $lines, (New-Object System.Text.UTF8Encoding $false))
    $p = Start-Process "$d\duckstation-qt-x64-ReleaseLTCG.exe" -ArgumentList "-bios" -WorkingDirectory $d -PassThru
    Start-Sleep -Seconds $Wait
    $log = Get-Content "$d\duckstation.log"
    $tty = ($log | Select-String "I/TTY").Count
    $c1 = $p.CPU; Start-Sleep 1; $p.Refresh(); $cpu = $p.CPU - $c1
    "$img : TTY lines $tty, cpu $cpu"
    Stop-Process -Id $p.Id -Force
    Start-Sleep -Seconds 1
}
