# RitoTex Photoshop Plugin - DEV Build Script
# Builds the DEBUG (Debug|x64) configuration of the RitoTex plugin.
#
# Why a separate dev build?
#   The Debug config defines _DEBUG, which makes the update checker BYPASS its
#   24h throttle - so every Photoshop launch hits GitHub and you can watch the
#   full update-check flow in DebugView (filter on "RitoTex/Update").
#   The Release config keeps the polite once-per-24h behavior.
#
# The Debug config has no post-build copy step, so this script copies the
# built RitoTex.8bi into Plugins\x64\ for you (same place build.ps1 puts it).

Write-Host "`n========================================" -ForegroundColor Magenta
Write-Host "  RitoTex Plugin Builder  [DEV / DEBUG]" -ForegroundColor Magenta
Write-Host "========================================`n" -ForegroundColor Magenta

# 1. Refresh PATH environment variables
Write-Host "[1/4] Refreshing environment variables..." -ForegroundColor Yellow
$env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path", "User")
Write-Host "      PATH refreshed" -ForegroundColor Green

# 2. Set Photoshop SDK path
Write-Host "`n[2/4] Setting up Photoshop SDK..." -ForegroundColor Yellow
$candidatePaths = @(
    $env:PHOTOSHOP_SDK_CS6,
    "$PSScriptRoot\PhotoshopSDK",
    "$PSScriptRoot\..\PhotoshopSDK"
)

$sdkPath = $null
foreach ($candidate in $candidatePaths) {
    if ($candidate -and (Test-Path (Join-Path $candidate "pluginsdk"))) {
        $sdkPath = (Resolve-Path $candidate).Path
        break
    }
}

if ($sdkPath) {
    $env:PHOTOSHOP_SDK_CS6 = $sdkPath
    Write-Host "      PHOTOSHOP_SDK_CS6 = $sdkPath" -ForegroundColor Green
} else {
    Write-Host "      [ERROR] Photoshop SDK (pluginsdk) not found in any of:" -ForegroundColor Red
    $candidatePaths | Where-Object { $_ } | ForEach-Object { Write-Host "        $_" -ForegroundColor Yellow }
    exit 1
}

# 3. Verify build tools
Write-Host "`n[3/4] Verifying build tools..." -ForegroundColor Yellow

$msbuildPath = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $msbuildPath)) {
    $msbuild = Get-Command msbuild -ErrorAction SilentlyContinue
    if ($msbuild) {
        $msbuildPath = $msbuild.Source
    } else {
        Write-Host "      [ERROR] MSBuild not found!" -ForegroundColor Red
        Write-Host "      Please run add-to-path-auto.ps1 first" -ForegroundColor Yellow
        exit 1
    }
}
Write-Host "      MSBuild: $msbuildPath" -ForegroundColor Green

# 4. Build the project (DEBUG)
Write-Host "`n[4/4] Building project..." -ForegroundColor Yellow
Write-Host "      Configuration: Debug  (_DEBUG -> update-check throttle bypassed)" -ForegroundColor Gray
Write-Host "      Platform: x64`n" -ForegroundColor Gray

$solutionPath = "$PSScriptRoot\IntelTextureWorks.sln"

if (-not (Test-Path $solutionPath)) {
    Write-Host "      [ERROR] Solution file not found: $solutionPath" -ForegroundColor Red
    exit 1
}

& $msbuildPath $solutionPath `
    /p:Configuration=Debug `
    /p:Platform=x64 `
    /m `
    /v:minimal `
    /nologo

if ($LASTEXITCODE -ne 0) {
    Write-Host "`n========================================" -ForegroundColor Magenta
    Write-Host "  DEV Build FAILED!" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Magenta
    Write-Host "`nCheck the errors above for details`n" -ForegroundColor Yellow
    exit 1
}

# The Debug config has no post-build copy step (unlike Release), so do it here.
$builtPlugin = "$PSScriptRoot\x64\Debug\RitoTex.8bi"
$pluginsDir  = "$PSScriptRoot\Plugins\x64"
$outputPlugin = "$pluginsDir\RitoTex.8bi"

if (-not (Test-Path $builtPlugin)) {
    Write-Host "`n[ERROR] Build reported success but $builtPlugin was not found." -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $pluginsDir)) {
    New-Item -ItemType Directory -Force -Path $pluginsDir | Out-Null
}
Copy-Item -Force $builtPlugin $outputPlugin

Write-Host "`n========================================" -ForegroundColor Magenta
Write-Host "  DEV Build SUCCESS!  (_DEBUG)" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Magenta
Write-Host "`nDebug plugin built at:" -ForegroundColor Green
Write-Host "  $outputPlugin`n" -ForegroundColor White

Write-Host "To install:" -ForegroundColor Cyan
Write-Host "  Copy to: C:\Program Files\Adobe\Adobe Photoshop [version]\Plug-ins\File Formats\" -ForegroundColor Gray

Write-Host "`nUpdate-check testing:" -ForegroundColor Cyan
Write-Host "  * This DEBUG build bypasses the 24h throttle - every Open/Save As checks GitHub." -ForegroundColor Gray
Write-Host "  * Run DebugView (as admin), enable 'Capture Global Win32', filter: RitoTex/Update" -ForegroundColor Gray
Write-Host "  * To simulate an available update, temporarily lower RITOTEX_VERSION_STR in IntelPluginName.h.`n" -ForegroundColor Gray

Write-Host "========================================`n" -ForegroundColor Magenta
