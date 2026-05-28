# RitoTex Photoshop Plugin - Build Script
# Builds the RitoTex plugin for Photoshop

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  RitoTex Plugin Builder" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

# 1. Refresh PATH environment variables
Write-Host "[1/4] Refreshing environment variables..." -ForegroundColor Yellow
$env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path", "User")
Write-Host "      PATH refreshed" -ForegroundColor Green

# 2. Set Photoshop SDK path
Write-Host "`n[2/4] Setting up Photoshop SDK..." -ForegroundColor Yellow
# The SDK lives one level up at <repo-root>\PhotoshopSDK, but allow an
# in-folder copy or a pre-set env var to take precedence.
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
    # Try to find MSBuild
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

# 4. Build the project
Write-Host "`n[4/4] Building project..." -ForegroundColor Yellow
Write-Host "      Configuration: Release" -ForegroundColor Gray
Write-Host "      Platform: x64`n" -ForegroundColor Gray

$solutionPath = "$PSScriptRoot\IntelTextureWorks.sln"

if (-not (Test-Path $solutionPath)) {
    Write-Host "      [ERROR] Solution file not found: $solutionPath" -ForegroundColor Red
    exit 1
}

# Build with MSBuild
& $msbuildPath $solutionPath `
    /p:Configuration=Release `
    /p:Platform=x64 `
    /m `
    /v:minimal `
    /nologo

if ($LASTEXITCODE -eq 0) {
    Write-Host "`n========================================" -ForegroundColor Cyan
    Write-Host "  Build SUCCESS!" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Cyan

    $outputPlugin = "$PSScriptRoot\Plugins\x64\RitoTex.8bi"
    if (Test-Path $outputPlugin) {
        Write-Host "`nPlugin built at:" -ForegroundColor Green
        Write-Host "  $outputPlugin`n" -ForegroundColor White

        Write-Host "To install:" -ForegroundColor Cyan
        Write-Host "  Copy to: C:\Program Files\Adobe\Adobe Photoshop [version]\Plug-ins\File Formats\" -ForegroundColor Gray
    }
} else {
    Write-Host "`n========================================" -ForegroundColor Cyan
    Write-Host "  Build FAILED!" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "`nCheck the errors above for details`n" -ForegroundColor Yellow
    exit 1
}

Write-Host "`n========================================`n" -ForegroundColor Cyan
