# Builds the redistributable Windows folder and zip from an existing Release build.
#
#   powershell -ExecutionPolicy Bypass -File scripts\package-release.ps1 -BuildDir build -Version 0.4.0
#
# Requirements: windeployqt (Qt bin) and g++ (MinGW bin) on PATH, OCR assets
# fetched with scripts\fetch_ocr_assets.ps1 before the build was configured.
param(
    [string]$BuildDir = 'build',
    [string]$OutDir = 'dist',
    [Parameter(Mandatory = $true)][string]$Version
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$output = Join-Path $root $OutDir
$name = "Visnip-$Version-windows-x64"
$package = Join-Path $output $name

$exe = Join-Path $build 'visnip.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "visnip.exe not found in $build" }
$ocr = Join-Path $build 'ocr'
if (-not (Test-Path -LiteralPath (Join-Path $ocr 'onnxruntime.dll'))) {
    throw 'OCR runtime missing: run scripts\fetch_ocr_assets.ps1, then configure and build again.'
}

Remove-Item -LiteralPath $package -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $package | Out-Null
Copy-Item -LiteralPath $exe -Destination $package

# Let windeployqt detect the build type (as package-release.cmd does); forcing
# --release rejects the MinGW Qt plugins and finds no platform plugin.
& windeployqt --compiler-runtime --no-translations --dir $package (Join-Path $package 'visnip.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with $LASTEXITCODE" }

# windeployqt only copies the compiler runtime it can locate; make it explicit.
$gccBin = Split-Path -Parent (Get-Command g++).Source
foreach ($dll in 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $package $dll))) {
        Copy-Item -LiteralPath (Join-Path $gccBin $dll) -Destination $package
    }
}

Copy-Item -LiteralPath $ocr -Destination (Join-Path $package 'ocr') -Recurse

foreach ($file in 'LICENSE', 'NOTICE', 'THIRD_PARTY_NOTICES.md', 'README.md', 'CHANGELOG.md') {
    Copy-Item -LiteralPath (Join-Path $root $file) -Destination $package
}
$licenses = Join-Path $package 'licenses'
New-Item -ItemType Directory -Force $licenses | Out-Null
Copy-Item -Path (Join-Path $root 'third_party\licenses\*') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $root 'third_party\onnxruntime\LICENSE') -Destination (Join-Path $licenses 'onnxruntime-LICENSE.txt')

# The package must start on a clean machine: Qt, platform plugin and runtime.
$selfTest = Start-Process -FilePath (Join-Path $package 'visnip.exe') -ArgumentList '--self-test' -Wait -PassThru -WindowStyle Hidden
if ($selfTest.ExitCode -ne 0) { throw "packaged visnip.exe --self-test failed with $($selfTest.ExitCode)" }
Remove-Item -LiteralPath (Join-Path $package 'logs') -Recurse -Force -ErrorAction SilentlyContinue

$zip = Join-Path $output "$name.zip"
Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $package '*') -DestinationPath $zip
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
"$hash  $name.zip" | Out-File -FilePath "$zip.sha256" -Encoding ascii
Write-Host "Package ready: $zip"
Write-Host "SHA-256: $hash"
