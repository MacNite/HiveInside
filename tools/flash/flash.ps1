<#
.SYNOPSIS
Flash a prebuilt HiveInside factory image onto a XIAO nRF54LM20A Sense.

.DESCRIPTION
Windows counterpart of flash.sh. It drives the board's on-board CMSIS-DAP
debugger with OpenOCD only — no Zephyr toolchain and no west workspace — and
runs the same OpenOCD sequence `west flash --verify` runs for this board, with
the board's RRAM loader corrected first: the upstream one leaves the RRAM write
buffer enabled and never commits it, so the tail of any image whose length is
not a multiple of 16 bytes is silently dropped and MCUboot then rejects slot 0.

.PARAMETER Image
The factory image to flash. Defaults to the single hiveinside-*-factory.hex in
the current directory, or next to this script.

.PARAMETER OpenocdCfg
Board OpenOCD config. Defaults to openocd\xiao_nrf54lm20a.cfg next to this
script, or to the one in a west workspace found from the current directory.

.PARAMETER Openocd
OpenOCD executable (default: openocd from PATH).

.PARAMETER NoVerify
Skip the read-back verification. Not recommended.

.EXAMPLE
.\flash.ps1
.EXAMPLE
.\flash.ps1 -Image hiveinside-nrf54lm20a-v0.5.0-lowpower-factory.hex
#>
[CmdletBinding()]
param(
    [string] $Image,
    [string] $OpenocdCfg,
    [string] $Openocd = 'openocd',
    [switch] $NoVerify
)

$ErrorActionPreference = 'Stop'
$scriptDir = $PSScriptRoot

function Die([string] $Message) {
    Write-Error $Message
    exit 1
}

# ── The image ──────────────────────────────────────────────────────────────
#
# Only the factory hex is flashable over SWD. The .signed.bin beside it is the
# BLE OTA payload — the application alone — so flashing that leaves nothing at
# 0x0 and the device goes completely silent.
if (-not $Image) {
    foreach ($dir in @($PWD.Path, $scriptDir)) {
        $found = @(Get-ChildItem -Path $dir -Filter 'hiveinside-*-factory.hex' -File -ErrorAction SilentlyContinue)
        if ($found.Count -gt 0) { break }
    }
    if ($found.Count -eq 0) {
        Die 'No hiveinside-*-factory.hex found here; pass one with -Image.'
    }
    if ($found.Count -gt 1) {
        Die ("Several factory images found; name the one to flash:`n  " +
             ($found.Name -join "`n  "))
    }
    $Image = $found[0].FullName
}

if ($Image -like '*.signed.bin') {
    Die ("$Image is the BLE OTA payload, not an SWD image.`n" +
         'Flash hiveinside-<version>-<variant>-factory.hex instead; upload the ' +
         '.signed.bin through HiveHub.')
}
if ($Image -notlike '*.hex') { Die "Expected a .hex factory image, got: $Image" }
if (-not (Test-Path -LiteralPath $Image)) { Die "No such file: $Image" }
$Image = (Resolve-Path -LiteralPath $Image).Path

if (-not (Get-Command $Openocd -ErrorAction SilentlyContinue)) {
    Die ("OpenOCD not found (looked for '$Openocd').`n" +
         'Install it (for example: winget install OpenOCD.OpenOCD, or the ' +
         'xPack OpenOCD build) or pass -Openocd C:\path\to\openocd.exe.')
}

# ── The board config ───────────────────────────────────────────────────────
if (-not $OpenocdCfg) {
    $bundled = Join-Path $scriptDir 'openocd\xiao_nrf54lm20a.cfg'
    if (Test-Path -LiteralPath $bundled) {
        $OpenocdCfg = $bundled
    } elseif (Get-Command west -ErrorAction SilentlyContinue) {
        $topdir = @(& west topdir 2>$null) | Select-Object -First 1
        if ($LASTEXITCODE -eq 0 -and $topdir) {
            $OpenocdCfg = Join-Path $topdir.Trim() `
                'zephyr\boards\seeed\xiao_nrf54lm20a\support\openocd.cfg'
        }
    }
}
if (-not $OpenocdCfg -or -not (Test-Path -LiteralPath $OpenocdCfg)) {
    Die ("No board OpenOCD config found; pass one with -OpenocdCfg.`n" +
         'It is boards/seeed/xiao_nrf54lm20a/support/openocd.cfg in a Zephyr ' +
         "checkout, and ships in openocd\ inside hiveinside-flash-tools.zip.")
}

# ── The RRAM write-buffer fix ──────────────────────────────────────────────
#
# 0x5004e500 is RRAMC.CONFIG. Upstream writes 0x101 — WEN=1 plus a one-line
# (16-byte) write buffer that the proc never commits, so a trailing partial
# line never reaches RRAM. 0x1 disables the buffer: every write commits.
# Slower, and correct.
$workdir = New-Item -ItemType Directory -Path (Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName()))
try {
    $cfgText = Get-Content -LiteralPath $OpenocdCfg -Raw
    if ($cfgText -match '0x5004e500 0x101') {
        $cfgText = $cfgText -replace 'mww 0x5004e500 0x101', 'mww 0x5004e500 0x1'
        Write-Host 'Applied the RRAM write-buffer fix to a temporary copy of the board config.'
    } elseif ($cfgText -match '0x5004e500 0x1\b') {
        Write-Host 'Board config already commits every RRAM write; using it unchanged.'
    } else {
        Die ("$OpenocdCfg has no recognisable nrf54lm20a-load RRAM setup.`n" +
             'Upstream may have restructured it — check it by hand before flashing.')
    }
    $cfg = Join-Path $workdir.FullName 'board.cfg'
    Set-Content -LiteralPath $cfg -Value $cfgText -NoNewline

    # OpenOCD passes the file name through Tcl, which splits on whitespace.
    if ($Image -match '\s') {
        $copy = Join-Path $workdir.FullName 'image.hex'
        Copy-Item -LiteralPath $Image -Destination $copy
        $Image = $copy
    }
    # Tcl also treats a backslash as an escape.
    $tclImage = $Image -replace '\\', '/'

    $openocdArgs = @(
        '-f', $cfg,
        '-c', 'init',
        '-c', 'targets nrf54lm20a.cpu',
        '-c', 'reset init',
        '-c', "nrf54lm20a-load $tclImage"
    )
    if (-not $NoVerify) {
        # The check that catches the truncation the fix above prevents.
        $openocdArgs += @('-c', 'reset init', '-c', "verify_image $tclImage")
    }
    $openocdArgs += @('-c', 'reset run', '-c', 'shutdown')

    Write-Host "Flashing $Image"
    & $Openocd @openocdArgs
    if ($LASTEXITCODE -ne 0) { Die "OpenOCD exited with $LASTEXITCODE" }
} finally {
    Remove-Item -Recurse -Force -LiteralPath $workdir.FullName -ErrorAction SilentlyContinue
}

Write-Host @'

Done. The node reboots into MCUboot and then the application.

A bringup image prints its banner on the board's USB serial port at 115200 8N1
(a COM port on Windows) — press RST with the monitor attached to see it.
A lowpower image has no console by design; look for its BLE advertisement
(HiveInside-XXXX) instead.
'@
