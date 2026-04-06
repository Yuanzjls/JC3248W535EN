# Regenerate documentation/fonts/UI_FONT_16.bin (LVGL v8) with Latin + CJK coverage.
# Requires: Node.js (npx). Downloads Noto Sans SC (OFL) on first run.
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$OutDir = Join-Path $Root "documentation\fonts"
$OutBin = Join-Path $OutDir "UI_FONT_16.bin"
$Otf = Join-Path $PSScriptRoot "NotoSansSC-Regular.otf"
$NotoUrl = "https://github.com/googlefonts/noto-cjk/raw/main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not (Test-Path $Otf)) {
    Write-Host "Downloading Noto Sans SC (Subset OTF)..."
    $ProgressPreference = "SilentlyContinue"
    Invoke-WebRequest -Uri $NotoUrl -OutFile $Otf -UseBasicParsing
}

Set-Location $Root
# Ranges: ASCII, CJK punctuation, fullwidth forms, full BMP CJK Unified Ideographs 0x4E00-0x9FFF (~21k chars).
# Narrower 0x4E00-0x5FFF misses common SSIDs (e.g. 阿 U+963F, 波 U+6CE2) while 尔/基/地 in the low band still work.
# Larger .bin and RAM at load time — copy regenerated UI_FONT_16.bin to the SD card.
npx --yes lv_font_conv `
    --font $Otf `
    --size 16 --bpp 4 `
    --format bin `
    -o $OutBin `
    -r 0x20-0x7E `
    -r 0x3000-0x303F `
    -r 0xFF00-0xFFEF `
    -r 0x4E00-0x9FFF

Write-Host "OK: $OutBin ($((Get-Item $OutBin).Length) bytes)"
Write-Host "Copy UI_FONT_16.bin to SD: fonts/UI_FONT_16.bin or /sd/UI_FONT_16.bin"
