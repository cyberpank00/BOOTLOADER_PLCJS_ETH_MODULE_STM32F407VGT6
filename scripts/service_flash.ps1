<#
    service_flash.ps1 - SERVICE / RMA path: program an application image through
    ST-Link and seed the matching bootloader metadata, so the bootloader accepts
    it without an OTA cycle.

    OTA is the only supported production path. Since bootloader 1.8 the boot
    decision runs a full CRC check of the installed image against the size and
    CRC stored in the sector-5 metadata, so an application written over SWD
    without matching metadata is refused (the board stays in the bootloader).
    This script writes both.

    What it touches:  sector 5 (metadata) and the application sectors needed by
                      the image (6-7).
    What it keeps:    bootloader (0-4), settings (10) and the 4RTD / 8AIC / 8AOC
                      write-once calibration (11). It never mass-erases.

    Order is power-cut safe: application first, metadata last. A cut in between
    leaves the old metadata, whose CRC no longer matches, so the board stays in
    the bootloader and can be recovered over OTA.

    The metadata fields (product id, hw revision, fw version) are read from the
    image's own fw_header_t (offset 0x200). The bootloader still rejects an image
    whose product id does not match its own build, so a wrong-variant image ends
    up in the bootloader, not running.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$AppBin,
    [string]$Cli,
    [string]$Serial,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'

$AppBase  = 0x08040000
$MetaBase = 0x08020000
$AppMax   = 256KB
$HdrOff   = 0x200

if (-not (Test-Path $AppBin)) { throw "Image not found: $AppBin" }
$bin = [IO.File]::ReadAllBytes((Resolve-Path $AppBin))

# --- image sanity (mirrors app_validate_vectors_at / app_validate_header) ------
if ($bin.Length -lt $HdrOff + 28 -or $bin.Length -gt $AppMax) { throw "Image size $($bin.Length) out of range" }
$msp   = [BitConverter]::ToUInt32($bin, 0)
$reset = [BitConverter]::ToUInt32($bin, 4)
if ($msp -le 0x20000000 -or $msp -gt 0x2001FFF0 -or ($msp -band 7) -ne 0) { throw ('Bad initial MSP 0x{0:X8}' -f $msp) }
$rt = [uint32]($reset -band [uint32]4294967294)
if (($reset -band 1) -eq 0 -or $rt -lt $AppBase -or ($rt - $AppBase) -gt ($bin.Length - 2)) { throw ('Bad reset vector 0x{0:X8}' -f $reset) }
$magic = [BitConverter]::ToUInt32($bin, $HdrOff)
if ($magic -ne 0x504C434A) { throw ('No fw_header_t at +0x200 (magic 0x{0:X8})' -f $magic) }
$product = [BitConverter]::ToUInt32($bin, $HdrOff + 4)
$hw      = [BitConverter]::ToUInt16($bin, $HdrOff + 8)
$fwver   = [BitConverter]::ToUInt32($bin, $HdrOff + 12)
$vto     = [BitConverter]::ToUInt32($bin, $HdrOff + 16)
if ($vto -ne 0) { throw 'fw_header vector_table_offset must be 0' }

Write-Host ('Image   : {0} ({1} bytes)' -f $AppBin, $bin.Length)
Write-Host ('Header  : product 0x{0:X8}  hw 0x{1:X4}  fw 0x{2:X4}' -f $product, $hw, $fwver)

# --- metadata ------------------------------------------------------------------
$meta = Join-Path ([IO.Path]::GetTempPath()) ('metadata_' + [Guid]::NewGuid().ToString('N') + '.bin')
& "$PSScriptRoot\make_metadata.ps1" -AppBin $AppBin -OutBin $meta `
    -ProductId ('0x{0:X8}' -f $product) -HwRevision ('0x{0:X4}' -f $hw) -FwVersion ('0x{0:X4}' -f $fwver) | Out-Null

# --- programmer ----------------------------------------------------------------
if (-not $Cli) {
    $c = Get-Command STM32_Programmer_CLI -ErrorAction SilentlyContinue
    if ($c) { $Cli = $c.Source }
    else {
        $Cli = Get-ChildItem 'C:\ST\STM32CubeCLT_*\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe' -ErrorAction SilentlyContinue |
               Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
    }
}
if (-not $Cli -or -not (Test-Path $Cli)) { throw 'STM32_Programmer_CLI not found; pass -Cli' }

$conn = @('-c', 'port=SWD', 'mode=UR', 'reset=HWrst')
if ($Serial) { $conn += "sn=$Serial" }
$cliArgs = $conn + @('-w', (Resolve-Path $AppBin).Path, ('0x{0:X8}' -f $AppBase), '-v',
                  '-w', $meta, ('0x{0:X8}' -f $MetaBase), '-v', '-rst')
Write-Host "Command : $Cli $($cliArgs -join ' ')"
if ($DryRun) { Write-Host 'Dry run: nothing programmed.'; Remove-Item $meta -ErrorAction SilentlyContinue; return }

& $Cli @cliArgs
$rc = $LASTEXITCODE
Remove-Item $meta -ErrorAction SilentlyContinue
if ($rc -ne 0) { throw "STM32_Programmer_CLI failed (exit $rc)" }
Write-Host 'Done. The application should start; confirm with PDP discovery.'

