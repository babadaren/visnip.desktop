# Downloads and verifies the binary assets required by fast translation.
# By default this installs the shared detector and the bundled PP-OCRv5 general
# recognition pack. Additional packs can be requested by stable ID:
#   powershell -ExecutionPolicy Bypass -File scripts/fetch_ocr_assets.ps1 -Packs general-v5,korean-v5

param(
    [string[]]$Packs = @('general-v5')
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ortVersion = '1.22.0'
$ortDir = Join-Path $root 'third_party\onnxruntime'
$ocrDir = Join-Path $root 'third_party\ocr'
$recDir = Join-Path $ocrDir 'rec'
$paddleRevision = '2661c7c0ef5c613e8f93c6e93b2e052399f0f854'
New-Item -ItemType Directory -Force "$ortDir\bin", "$ortDir\include", $ocrDir, $recDir | Out-Null

$catalog = @{
    'general-v5' = @{
        Repository = 'PP-OCRv5_mobile_rec_onnx'
        Revision = 'ed152b8b495f84de93cda5709d768548a9127622'
        ModelHash = 'da72dc72ca4dc220df0dfde68c1dedc31c58d3e76a25871122e5056227d50092'
        ModelBytes = 16534782
        Dictionary = 'ppocrv5_dict.txt'
        DictionaryHash = 'd1979e9f794c464c0d2e0b70a7fe14dd978e9dc644c0e71f14158cdf8342af1b'
    }
    'korean-v5' = @{
        Repository = 'korean_PP-OCRv5_mobile_rec_onnx'
        Revision = '5c6f574b8e2230adf4287b33e736d71b9fabd28e'
        ModelHash = '92f0b7785e64fc9090106a241cf4c1eb97472824558272751b88a2a4476d3a08'
        ModelBytes = 13418787
        Dictionary = 'ppocrv5_korean_dict.txt'
        DictionaryHash = 'a88071c68c01707489baa79ebe0405b7beb5cca229f4fc94cc3ef992328802d7'
    }
    'latin-v5' = @{
        Repository = 'latin_PP-OCRv5_mobile_rec_onnx'
        Revision = '89d3a50e2c27e2e7cceeab0e944c25c807d5db4f'
        ModelHash = '7888113072263cb471b93f66dd5e2ad70548dc526fa1ace760d0d973dd121498'
        ModelBytes = 8042023
        Dictionary = 'ppocrv5_latin_dict.txt'
        DictionaryHash = 'ccbcc45730b3fbbd9050c5bc74db6a99067141ef1035e3d14889a84a6b9b1aff'
    }
    'cyrillic-v5' = @{
        Repository = 'cyrillic_PP-OCRv5_mobile_rec_onnx'
        Revision = '2cef88145434beb8afa9dd82d77d799eb1ad7b29'
        ModelHash = '5371ee1ddaa7983cc62d0818d99e982b6804638c85e4f960d59a574094e172e5'
        ModelBytes = 8048799
        Dictionary = 'ppocrv5_cyrillic_dict.txt'
        DictionaryHash = 'db40aa52ceb112055be80c694afdf655d5d2c4f7873704524cc16a447ca913ba'
    }
    'arabic-v5' = @{
        Repository = 'arabic_PP-OCRv5_mobile_rec_onnx'
        Revision = '14aaedcd75825982689ecf5cd64ab33ee083215a'
        ModelHash = '799113ebf267fbe742deb99eb36e8d42c9ddc5291ceacf92add41b4d52a59110'
        ModelBytes = 7998947
        Dictionary = 'ppocrv5_arabic_dict.txt'
        DictionaryHash = '7f92f7dbb9b75a4787a83bfb4f6d14a8ab515525130c9d40a9036f61cf6999e9'
    }
}

function Test-VerifiedFile($Path, [long]$ExpectedBytes, [string]$ExpectedHash) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    if ($ExpectedBytes -gt 0 -and (Get-Item -LiteralPath $Path).Length -ne $ExpectedBytes) { return $false }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $ExpectedHash
}

function Fetch-Verified($Urls, $Destination, [long]$ExpectedBytes, [string]$ExpectedHash) {
    if (Test-VerifiedFile $Destination $ExpectedBytes $ExpectedHash) {
        Write-Host "verified: $Destination"
        return
    }
    $temporary = "$Destination.download"
    Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    foreach ($url in $Urls) {
        try {
            Write-Host "fetch: $url"
            curl.exe -fL --retry 3 --connect-timeout 20 --max-time 300 -o $temporary $url
            if ($LASTEXITCODE -ne 0) { throw "curl exited with $LASTEXITCODE" }
            if (-not (Test-VerifiedFile $temporary $ExpectedBytes $ExpectedHash)) {
                throw 'size or SHA-256 mismatch'
            }
            Move-Item -LiteralPath $temporary -Destination $Destination -Force
            return
        } catch {
            Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
            Write-Warning "download attempt failed: $url ($($_.Exception.Message))"
        }
    }
    throw "all downloads failed: $Destination"
}

$detectorRevision = '4e4644045a07c403b1ad40ca97340ecb4a8dc2c1'
$detectorSuffix = "/SWHL/RapidOCR/resolve/$detectorRevision/PP-OCRv4/ch_PP-OCRv4_det_infer.onnx"
Fetch-Verified @(
    "https://huggingface.co$detectorSuffix",
    "https://hf-mirror.com$detectorSuffix"
) (Join-Path $ocrDir 'ch_PP-OCRv4_det_infer.onnx') 4745517 'd2a7720d45a54257208b1e13e36a8479894cb74155a5efe29462512d42f49da9'

foreach ($packId in $Packs) {
    $packId = $packId.Trim().ToLowerInvariant()
    if (-not $catalog.ContainsKey($packId)) { throw "unknown OCR pack: $packId" }
    $pack = $catalog[$packId]
    $packDir = Join-Path $recDir $packId
    New-Item -ItemType Directory -Force $packDir | Out-Null
    $modelSuffix = "/PaddlePaddle/$($pack.Repository)/resolve/$($pack.Revision)/inference.onnx"
    Fetch-Verified @(
        "https://huggingface.co$modelSuffix",
        "https://hf-mirror.com$modelSuffix"
    ) (Join-Path $packDir 'model.onnx') $pack.ModelBytes $pack.ModelHash
    $dictionaryPath = "PaddlePaddle/PaddleOCR/$paddleRevision/ppocr/utils/dict/$($pack.Dictionary)"
    Fetch-Verified @(
        "https://raw.githubusercontent.com/$dictionaryPath",
        "https://cdn.jsdelivr.net/gh/PaddlePaddle/PaddleOCR@$paddleRevision/ppocr/utils/dict/$($pack.Dictionary)"
    ) (Join-Path $packDir $pack.Dictionary) 0 $pack.DictionaryHash
}

if (-not (Test-Path "$ortDir\bin\onnxruntime.dll")) {
    $zip = Join-Path $env:TEMP "onnxruntime-win-x64-$ortVersion.zip"
    $extract = Join-Path $env:TEMP "onnxruntime-extract-$ortVersion"
    Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction SilentlyContinue
    curl.exe -fL --retry 3 --connect-timeout 20 -o $zip "https://github.com/microsoft/onnxruntime/releases/download/v$ortVersion/onnxruntime-win-x64-$ortVersion.zip"
    if ($LASTEXITCODE -ne 0) { throw 'onnxruntime download failed' }
    Expand-Archive -Force $zip $extract
    $source = Join-Path $extract "onnxruntime-win-x64-$ortVersion"
    Copy-Item "$source\lib\onnxruntime.dll" "$ortDir\bin\" -Force
    Copy-Item "$source\lib\onnxruntime_providers_shared.dll" "$ortDir\bin\" -Force
    Copy-Item "$source\include\onnxruntime_c_api.h" "$ortDir\include\" -Force
    Copy-Item "$source\LICENSE" "$ortDir\LICENSE" -Force
    Copy-Item "$source\VERSION_NUMBER" "$ortDir\VERSION_NUMBER" -Force
    Remove-Item -LiteralPath $extract -Recurse -Force
    Remove-Item -LiteralPath $zip -Force
} else {
    Write-Host "verified existing runtime: $ortDir\bin\onnxruntime.dll"
}

Write-Host "OCR assets ready: $($Packs -join ', ')"
