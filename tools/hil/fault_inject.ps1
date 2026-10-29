#requires -Version 5.1
<#
    fault_inject.ps1 - hardware-in-the-loop check that the bootloader refuses an
    application that is damaged while the sector-5 metadata still says "valid".

    It programs a damaged application over ST-Link, resets the board and reports
    whether the module came back as an APPLICATION (bad: damaged image started) or
    stayed in the BOOTLOADER (expected). Recover afterwards with an OTA update.

    -Mode corrupt   one byte of the image flipped (CRC mismatch)
    -Mode erase     application sector 6 erased
    -Mode half      only the first half of the image programmed

    Only sector 6 is touched; the bootloader, metadata, settings and the
    write-once calibration sector 11 are not. Needs the module's MAC (PDP) and the
    host NIC address on the module's subnet.

    NOTE: an SWD reset is serviced only after a running Flash erase/program
    finishes, so this cannot imitate a real power loss in the middle of one; it
    checks the boot decision for the resulting states.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('corrupt', 'erase', 'half')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$Mac,
    [Parameter(Mandatory = $true)][string]$Nic,
    [string]$AppBin,
    [string]$Cli
)
$ErrorActionPreference = 'Stop'
if (-not $Cli) {
    $Cli = Get-ChildItem 'C:\ST\STM32CubeCLT_*\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe' -ErrorAction SilentlyContinue |
           Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Cli) { throw 'STM32_Programmer_CLI not found; pass -Cli' }
if ($Mode -ne 'erase' -and -not (Test-Path $AppBin)) { throw '-AppBin is required for corrupt/half' }

$conn = @('-c', 'port=SWD', 'mode=UR', 'reset=HWrst')
switch ($Mode) {
    'erase'   { & $Cli @conn -e 6 -rst | Out-Null }
    'corrupt' {
        $b = [IO.File]::ReadAllBytes((Resolve-Path $AppBin)); $b[0x4000] = $b[0x4000] -bxor 0xFF
        $tmp = Join-Path ([IO.Path]::GetTempPath()) 'app_corrupt.bin'; [IO.File]::WriteAllBytes($tmp, $b)
        & $Cli @conn -w $tmp 0x08040000 -v -rst | Out-Null
    }
    'half'    {
        $b = [IO.File]::ReadAllBytes((Resolve-Path $AppBin))
        $tmp = Join-Path ([IO.Path]::GetTempPath()) 'app_half.bin'; [IO.File]::WriteAllBytes($tmp, $b[0..([int]($b.Length / 2) - 1)])
        & $Cli @conn -w $tmp 0x08040000 -v -rst | Out-Null
    }
}
if ($LASTEXITCODE -ne 0) { throw "ST-Link programming failed (exit $LASTEXITCODE)" }

Start-Sleep 9
$scan = & node "$PSScriptRoot\pdp_scan.mjs" $Nic 3000 | Select-String $Mac.ToLower().Replace('02:00:', '')
if (-not $scan)                      { "RESULT: module not visible" }
elseif ($scan -match '"inBL":true')  { "RESULT: PASS - module stayed in the bootloader ($Mode)" }
else                                 { "RESULT: FAIL - the damaged application was started ($Mode)" }
