#requires -Version 5.1
<#
    hil_session_timeout.ps1 - hardware-in-the-loop check of the abandoned-OTA-session
    timeout (bootloader 1.9+).

    Starts tools/fw_update.mjs against a module, kills the client part-way through
    the transfer, then polls PDP until the module is back in its application. The
    expected time is about FW_SESSION_TIMEOUT_MS (120 s) plus the reboot.

    The module must already be in the bootloader with a valid application, and the
    NIC must be on the module's subnet. A module without a valid application
    stays in the bootloader (that is the correct result, reported as such).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Mac,
    [Parameter(Mandatory = $true)][string]$Nic,
    [Parameter(Mandatory = $true)][string]$Bin,
    [string]$BootIp = '192.168.1.2',
    [int]$KillAtBlock = 100
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
$out  = Join-Path ([IO.Path]::GetTempPath()) 'hil_timeout_out.txt'
Remove-Item $out -ErrorAction SilentlyContinue

function Read-Shared([string]$path) {
    try {
        $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite'); $sr = New-Object IO.StreamReader($fs)
        $t = $sr.ReadToEnd(); $sr.Close(); $t
    } catch { '' }
}

$pr = Start-Process node -ArgumentList "`"$root\tools\fw_update.mjs`" update `"$Bin`" --ip $BootIp --mac $Mac --nic $Nic" `
        -RedirectStandardOutput $out -NoNewWindow -PassThru
$sw = [Diagnostics.Stopwatch]::StartNew()
while (-not $pr.HasExited -and $sw.Elapsed.TotalSeconds -lt 90) {
    if ((Read-Shared $out) -match "\s$KillAtBlock/\d+") { break }
    Start-Sleep -Milliseconds 50
}
if ($pr.HasExited) { throw 'client finished before it could be killed' }
$pr.Kill()
"client killed after block ~$KillAtBlock"

$t0 = Get-Date
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep 10
    $scan  = & node "$PSScriptRoot\pdp_scan.mjs" $Nic 2000 | Select-String $Mac.ToLower().Replace('02:00:', '')
    $state = if ($scan -match '"inBL":true') { 'BOOTLOADER' } elseif ($scan) { 'APPLICATION' } else { 'not visible' }
    '{0,4:N0} s after kill: {1}' -f ((Get-Date) - $t0).TotalSeconds, $state
    if ($state -eq 'APPLICATION') { 'RESULT: PASS - session timed out, application started'; return }
}
'RESULT: still in the bootloader after the window (expected only if there is no valid application)'
