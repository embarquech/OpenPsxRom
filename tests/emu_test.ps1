# Level 2 tests: build/openrom.rom booted in DuckStation (Xplorer cartridge emulation).
#
#   powershell -ExecutionPolicy Bypass -File tests\emu_test.ps1 [-Duck DIR] [-Rom2 FILE] [-Wait 12]
#
# -Duck: a DuckStation folder with a BIOS, configured for Xplorer cartridge emulation
#        (settings.ini: FlashImagePath, SwitchActive), default ..\..\tools\duckstation2
# -Rom2: a 128 KB firmware built for bank 2 (1F020000h, e.g. Unirom "ROM2"); its test is skipped
#        without it. Checks the TTY log (I/TTY lines of duckstation.log) of each boot.
param([string]$Duck = "$PSScriptRoot\..\..\tools\duckstation2", [string]$Rom2 = "", [int]$Wait = 12)
$ErrorActionPreference = "Stop"
$romFile = "$PSScriptRoot\..\build\openrom.rom"
if (-not (Test-Path $romFile)) { Write-Host "build/openrom.rom missing: run make first"; exit 2 }
if (-not (Test-Path "$Duck\settings.ini")) { Write-Host "no DuckStation at $Duck"; exit 2 }
$exe = Get-ChildItem "$Duck\duckstation*.exe" | Select-Object -First 1
$rom = [IO.File]::ReadAllBytes($romFile)
$pass = 0; $fail = 0; $skip = 0

function Stop-Duck {
    foreach ($q in Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith((Resolve-Path $Duck).Path) }) {
        $q.CloseMainWindow() | Out-Null
        if (-not $q.WaitForExit(10000)) { $q | Stop-Process -Force; Start-Sleep -Seconds 2 }
    }
}
# image: OpenPsxRom in bank 1, optional ROM 2 in bank 2, settings page 'OCF1' + flags at 1FF80h
function Invoke-Boot([int]$Flag = -1, [byte[]]$Bank2 = $null, [bool]$SwitchOn = $true) {
    Stop-Duck
    $img = New-Object byte[] 262144
    for ($i = 0; $i -lt $img.Length; $i++) { $img[$i] = 0xFF }
    [Array]::Copy($rom, 0, $img, 0, 131072)
    if ($Bank2) { [Array]::Copy($Bank2, 0, $img, 131072, 131072) }
    if ($Flag -ge 0) {
        [Array]::Copy([BitConverter]::GetBytes([uint32]0x3146434F), 0, $img, 0x1FF80, 4)
        [Array]::Copy([BitConverter]::GetBytes([uint32]$Flag), 0, $img, 0x1FF84, 4)
    }
    [IO.File]::WriteAllBytes("$Duck\openrom256.rom", $img)
    $sw = if ($SwitchOn) { "true" } else { "false" }
    $lines = (Get-Content "$Duck\settings.ini") -replace '^FlashImagePath = .*', "FlashImagePath = openrom256.rom" -replace '^SwitchActive = .*', "SwitchActive = $sw"
    [IO.File]::WriteAllLines("$Duck\settings.ini", $lines, (New-Object Text.UTF8Encoding $false))
    Remove-Item "$Duck\duckstation.log" -ErrorAction SilentlyContinue
    Start-Process $exe.FullName -ArgumentList "-bios" -WorkingDirectory $Duck | Out-Null
    Start-Sleep -Seconds $Wait
    $tty = @(Select-String -Path "$Duck\duckstation.log" -Pattern "I/TTY: " | ForEach-Object { $_.Line -replace '^.*I/TTY: ', '' })
    Stop-Duck
    return ,$tty
}
function Test-Case([string]$Name, [string[]]$Tty, [string[]]$Want, [string[]]$NotWant = @()) {
    $text = $Tty -join "`n"
    $miss = @($Want | Where-Object { $text -notmatch [regex]::Escape($_) })
    $bad = @($NotWant | Where-Object { $text -match [regex]::Escape($_) })
    if ($miss.Count -eq 0 -and $bad.Count -eq 0) { Write-Host "PASS  $Name" -ForegroundColor Green; $script:pass++ }
    else {
        Write-Host "FAIL  $Name" -ForegroundColor Red; $script:fail++
        foreach ($m in $miss) { Write-Host "        missing: $m" }
        foreach ($b in $bad) { Write-Host "        unexpected: $b" }
        $Tty | Select-Object -Last 12 | ForEach-Object { Write-Host "        | $_" }
    }
}

# 1. switch ON: OpenPsxRom starts and prints its whole boot log
$t = Invoke-Boot -SwitchOn $true
Test-Case "switch ON: OpenPsxRom boots, boot log complete" $t @(
    "OpenPsxRom 0.6 (", "BIOS: System ROM Version", "Video: ", "Flash: ", "Flash @40000h: ",
    "Cheats: ", "Switch: on, off at power-on = bypass", "Pad 1: ", "Card 1: ", "Card 2: ", "CD: ",
    "Serial: 115200", "OpenPsxRom 0.6 ready") @("CRASH")

# 2. switch OFF, no settings page: bypass (the cartridge stays invisible, normal BIOS boot)
$t = Invoke-Boot -SwitchOn $false
Test-Case "switch OFF, default: bypass" $t @("System ROM Version") @("OpenPsxRom")

# 3. switch OFF, bank 3 chosen but no firmware there: refused, bypass
$t = Invoke-Boot -Flag 3 -SwitchOn $false
Test-Case "switch OFF, bank 3 without firmware: bypass" $t @("System ROM Version") @("OpenPsxRom")

# 4. switch OFF, invalid setting value: bypass
$t = Invoke-Boot -Flag 0x7F -SwitchOn $false
Test-Case "switch OFF, invalid setting: bypass" $t @("System ROM Version") @("OpenPsxRom")

# 5. switch OFF, bank 2 chosen but empty: refused, bypass
$t = Invoke-Boot -Flag 2 -SwitchOn $false
Test-Case "switch OFF, bank 2 empty: bypass" $t @("System ROM Version") @("OpenPsxRom")

# 6. switch OFF, bank 2 with a ROM 2 firmware: it starts
if ($Rom2 -and (Test-Path $Rom2)) {
    $b2 = [IO.File]::ReadAllBytes($Rom2)
    $t = Invoke-Boot -Flag 2 -Bank2 $b2 -SwitchOn $false
    # neither OpenPsxRom nor the BIOS shell (normal boot) may run: the bank 2 firmware took over
    Test-Case "switch OFF, bank 2 firmware: started" $t @("KERNEL SETUP") @("OpenPsxRom", "System ROM Version")
    $t = Invoke-Boot -Flag 1 -Bank2 $b2 -SwitchOn $false
    Test-Case "switch OFF, former 'ROM 2' setting (1): bank 2 started" $t @("KERNEL SETUP") @("OpenPsxRom", "System ROM Version")
    $t = Invoke-Boot -Flag 2 -Bank2 $b2 -SwitchOn $true
    Test-Case "switch ON with a bank 2 firmware: still OpenPsxRom" $t @("OpenPsxRom 0.6 ready", "firmware in bank 2: yes")
} else { Write-Host "SKIP  bank 2 firmware tests (give -Rom2 FILE)" -ForegroundColor Yellow; $script:skip++ }

$lines = (Get-Content "$Duck\settings.ini") -replace '^SwitchActive = .*', "SwitchActive = true"
[IO.File]::WriteAllLines("$Duck\settings.ini", $lines, (New-Object Text.UTF8Encoding $false))
Write-Host ""
Write-Host "$pass passed, $fail failed, $skip skipped"
exit ([int]($fail -gt 0))
