# Build crosspoint.wfx + crosspoint.wfx64 on Windows using MSYS2.
#
# Prerequisites (one-time setup):
#   1. Install MSYS2 from https://www.msys2.org/
#   2. Open any MSYS2 shell and run:
#        pacman -S mingw-w64-x86_64-gcc mingw-w64-i686-gcc make zip
#
# Then just run this script (right-click -> Run with PowerShell, or from a
# PowerShell prompt: .\build.ps1).
# Output: ..\dist\crosspoint-usb-wfx-windows.zip

# Pause at the end only when double-clicked (no controlling console process
# other than this one) so the script stays usable in CI / non-interactive runs.
$Interactive = [Environment]::UserInteractive -and -not ([Console]::IsInputRedirected)

function Exit-WithPause([int]$Code) {
    if ($Interactive) { Read-Host "Press Enter to exit" | Out-Null }
    exit $Code
}

# Locate MSYS2. Adjust this path if you installed MSYS2 elsewhere.
$Msys2 = "C:\msys64"
$Bash = Join-Path $Msys2 "usr\bin\bash.exe"
if (-not (Test-Path $Bash)) {
    Write-Host "ERROR: MSYS2 not found at $Msys2"
    Write-Host "Install from https://www.msys2.org/ then re-run this script."
    Exit-WithPause 1
}

$Cygpath = Join-Path $Msys2 "usr\bin\cygpath.exe"
$WfxWin = $PSScriptRoot
$WfxDir = (& $Cygpath -u "$WfxWin").Trim() + "/"

function Invoke-MsysMake {
    param(
        [string]$MingwDir,
        [string]$NativeOut
    )
    & $Bash --norc -c "PATH=/$MingwDir/bin:/usr/bin make -C '$WfxDir' dist-windows-native NATIVE_OUT=$NativeOut" | Write-Host
    return $LASTEXITCODE
}

Write-Host "--- Building crosspoint.wfx64 (64-bit) ---"
if ((Invoke-MsysMake -MingwDir "mingw64" -NativeOut "crosspoint.wfx64") -ne 0) {
    Write-Host "FAILED (64-bit build). Is mingw-w64-x86_64-gcc installed?"
    Exit-WithPause 1
}

Write-Host "--- Building crosspoint.wfx (32-bit) ---"
if ((Invoke-MsysMake -MingwDir "mingw32" -NativeOut "crosspoint.wfx") -ne 0) {
    Write-Host "FAILED (32-bit build). Is mingw-w64-i686-gcc installed?"
    Exit-WithPause 1
}

Write-Host "--- Packaging ---"
$DistWin = Join-Path $PSScriptRoot "..\dist\win"
Copy-Item -Force (Join-Path $PSScriptRoot "pluginst.inf") $DistWin
Copy-Item -Force (Join-Path $PSScriptRoot "..\README.MD") (Join-Path $DistWin "README.md")

$ZipPath = Join-Path $PSScriptRoot "..\dist\crosspoint-usb-wfx-windows.zip"
Remove-Item -Force -ErrorAction SilentlyContinue $ZipPath

$ZipItems = @(
    (Join-Path $DistWin "crosspoint.wfx"),
    (Join-Path $DistWin "crosspoint.wfx64"),
    (Join-Path $DistWin "pluginst.inf"),
    (Join-Path $DistWin "README.md")
)
Compress-Archive -Path $ZipItems -DestinationPath $ZipPath

Write-Host ""
Write-Host "Done: $ZipPath"
Exit-WithPause 0
