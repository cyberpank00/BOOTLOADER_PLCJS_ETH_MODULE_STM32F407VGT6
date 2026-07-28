<#
.SYNOPSIS
    Build the PLCJS bootloader for every hardware variant from a single source
    tree. Each variant differs only in the PRODUCT_ID and HW_REVISION identity
    constants (the firmware itself is identical across all boards).

.DESCRIPTION
    Reads scripts/variants.csv (name, product_id, hw_revision, description) and,
    for each row, configures an isolated CMake build directory
    (build/<name>-<config>) with the matching -DPRODUCT_ID / -DHW_REVISION cache
    variables, builds it, and copies the resulting .hex/.bin into dist/ with a
    descriptive name.

.PARAMETER Variant
    Build only the named variant (e.g. -Variant 4rtd). Default: all variants.

.PARAMETER Config
    CMake build type. Default: Release.

.PARAMETER Clean
    Delete each variant's build directory before configuring (fresh build).

.PARAMETER CltRoot
    STM32CubeCLT install root. Auto-detected if not given.

.EXAMPLE
    ./scripts/build_all.ps1
    ./scripts/build_all.ps1 -Variant 4rtd -Clean
    ./scripts/build_all.ps1 -Config Debug
#>
[CmdletBinding()]
param(
    [string]$Variant = "",
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [switch]$Clean,
    [string]$CltRoot = ""
)

$ErrorActionPreference = "Stop"

# --- Locate the repository root (parent of this script's folder) -------------
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Toolchain = Join-Path $RepoRoot "arm-none-eabi-toolchain.cmake"
$VariantsCsv = Join-Path $PSScriptRoot "variants.csv"
$DistDir = Join-Path $RepoRoot "dist"

# --- Locate STM32CubeCLT (cmake, ninja, arm-none-eabi-gcc) -------------------
if (-not $CltRoot) {
    $candidates = Get-ChildItem "C:\ST" -Directory -Filter "STM32CubeCLT_*" -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
    if ($candidates) { $CltRoot = $candidates[0].FullName }
}
if (-not $CltRoot -or -not (Test-Path $CltRoot)) {
    throw "STM32CubeCLT not found. Pass -CltRoot <path> explicitly."
}

$CMake   = Join-Path $CltRoot "CMake\bin\cmake.exe"
$NinjaBin = Join-Path $CltRoot "Ninja\bin"
$GccBin  = Join-Path $CltRoot "GNU-tools-for-STM32\bin"
foreach ($p in @($CMake, (Join-Path $NinjaBin "ninja.exe"), (Join-Path $GccBin "arm-none-eabi-gcc.exe"))) {
    if (-not (Test-Path $p)) { throw "Required tool not found: $p" }
}
# Make ninja + arm-none-eabi-* visible to CMake/toolchain for this process.
$env:PATH = "$GccBin;$NinjaBin;" + $env:PATH

# --- Load and filter the variant table ---------------------------------------
if (-not (Test-Path $VariantsCsv)) { throw "Variant table not found: $VariantsCsv" }
$variants = Import-Csv $VariantsCsv
if ($Variant) {
    $variants = $variants | Where-Object { $_.name -eq $Variant }
    if (-not $variants) { throw "Variant '$Variant' not found in $VariantsCsv" }
}

New-Item -ItemType Directory -Force -Path $DistDir | Out-Null

# Decode HW revision 0xMMmmpp -> "MM.mm.pp" for the output file name.
function Format-HwRev([string]$hex) {
    $v = [Convert]::ToUInt32($hex, 16)
    "{0:00}.{1:00}.{2:00}" -f (($v -shr 16) -band 0xFF), (($v -shr 8) -band 0xFF), ($v -band 0xFF)
}

$results = @()
foreach ($v in $variants) {
    $name   = $v.name.Trim()
    $prodId = $v.product_id.Trim()
    $hw     = $v.hw_revision.Trim()
    $buildDir = Join-Path $RepoRoot "build\$name-$Config"

    Write-Host ""
    Write-Host "==== $name  (PRODUCT_ID=$prodId  HW_REVISION=$hw) ====" -ForegroundColor Cyan

    if ($Clean -and (Test-Path $buildDir)) {
        Remove-Item $buildDir -Recurse -Force
    }

    & $CMake -B $buildDir -G Ninja `
        "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
        "-DCMAKE_BUILD_TYPE=$Config" `
        "-DPRODUCT_ID=$prodId" `
        "-DHW_REVISION=$hw" `
        $RepoRoot
    if ($LASTEXITCODE -ne 0) { throw "Configure failed for $name" }

    & $CMake --build $buildDir
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $name" }

    $hwStr = Format-HwRev $hw
    $srcHex = Join-Path $buildDir "BOOTLOADER_PLCJS.hex"
    $srcBin = Join-Path $buildDir "BOOTLOADER_PLCJS.bin"
    $dstHex = Join-Path $DistDir "BOOTLOADER_PLCJS_${name}_hw${hwStr}.hex"
    $dstBin = Join-Path $DistDir "BOOTLOADER_PLCJS_${name}_hw${hwStr}.bin"
    Copy-Item $srcHex $dstHex -Force
    if (Test-Path $srcBin) { Copy-Item $srcBin $dstBin -Force }

    $results += [pscustomobject]@{
        Variant   = $name
        ProductID = $prodId
        HW        = $hwStr
        Output    = Split-Path $dstHex -Leaf
    }
}

Write-Host ""
Write-Host "==== Build summary ($Config) ====" -ForegroundColor Green
$results | Format-Table -AutoSize
Write-Host "Artifacts in: $DistDir"
