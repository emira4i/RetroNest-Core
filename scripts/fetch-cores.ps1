<#
.SYNOPSIS
    Downloads every bundled Libretro core (arm64-v8a) listed in app/src/main/assets/cores_catalog.json.

.DESCRIPTION
    For each catalog entry downloads
      https://buildbot.libretro.com/nightly/android/latest/arm64-v8a/<id>_libretro_android.so.zip
    and extracts it to corehost/libs/arm64-v8a/lib<id>_libretro_android.so. Existing files are skipped.
    Entries with "source": "build" are built locally and skipped here: mame and mame2010
    (tools/arcade/), and the cores pinned in tools/cores/pins.json (tools/cores/build_cores.sh).
    Also downloads the PPSSPP system asset pack (fonts, compat.ini...) into
    corehost/src/main/assets/ppsspp/PPSSPP (only the files the libretro core needs; the core host
    copies them to the system folder when a PSP game starts). Skipped if present.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\fetch-cores.ps1
    powershell -ExecutionPolicy Bypass -File scripts\fetch-cores.ps1 -Force   # re-download everything
#>
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root    = Split-Path -Parent $PSScriptRoot
$Catalog = Join-Path $Root 'app\src\main\assets\cores_catalog.json'
$OutDir  = Join-Path $Root 'corehost\libs\arm64-v8a'
$BaseUrl = 'https://buildbot.libretro.com/nightly/android/latest/arm64-v8a'

function Write-Ok($msg)   { Write-Host "    [OK]   $msg" -ForegroundColor Green }
function Write-Fail($msg) { Write-Host "    [FAIL] $msg" -ForegroundColor Red }

if (-not (Test-Path $Catalog)) { throw "Catalog not found: $Catalog" }
$cores = (Get-Content $Catalog -Raw | ConvertFrom-Json).cores
$Pins = Join-Path $Root 'tools\cores\pins.json'
$pinned = @((Get-Content $Pins -Raw | ConvertFrom-Json).cores.PSObject.Properties.Name)
$BuildScripts = @{
    'mame'     = 'tools/arcade/build_mame_current.sh'
    'mame2010' = 'tools/arcade/build_mame2010.sh <work dir>'
}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$failed = @()
foreach ($core in $cores) {
    $libName = if ($core.libName) { $core.libName } else { "lib$($core.id)_libretro_android.so" }
    $target  = Join-Path $OutDir $libName
    Write-Host "`n==> $($core.displayName) ($($core.id))" -ForegroundColor Cyan
    if ($core.source -eq 'build') {
        $script = if ($pinned -contains $core.id) { "tools/cores/build_cores.sh $($core.id)" }
                  elseif ($BuildScripts.ContainsKey($core.id)) { $BuildScripts[$core.id] }
                  else { throw "$($core.id) is marked source=build but no build script is known for it." }
        $state = if (Test-Path $target) { 'present' } else { 'MISSING' }
        Write-Host "    [SKIP] built locally, not downloaded ($state): run $script in WSL (see the script header)" -ForegroundColor Yellow
        continue
    }
    if ((Test-Path $target) -and -not $Force) {
        Write-Ok 'Already present (use -Force to re-download)'
        continue
    }
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("retronest-core-" + [Guid]::NewGuid())
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        $zip = Join-Path $tmp 'core.zip'
        Invoke-WebRequest -Uri "$BaseUrl/$($core.id)_libretro_android.so.zip" -OutFile $zip -UseBasicParsing
        Expand-Archive -Path $zip -DestinationPath $tmp -Force
        $so = Get-ChildItem $tmp -Filter '*.so' -Recurse | Select-Object -First 1
        if (-not $so) { throw 'No .so found inside the core archive.' }
        Move-Item -Path $so.FullName -Destination $target -Force
        Write-Ok ("Saved {0} ({1:N0} KB)" -f $target, ((Get-Item $target).Length / 1KB))
    } catch {
        Write-Fail $_.Exception.Message
        $failed += $core.id
    } finally {
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# --- PPSSPP system assets -----------------------------------------------------------------
$PspDir = Join-Path $Root 'corehost\src\main\assets\ppsspp\PPSSPP'
Write-Host "`n==> PPSSPP system assets" -ForegroundColor Cyan
if ((Test-Path (Join-Path $PspDir 'flash0\font\ltn0.pgf')) -and -not $Force) {
    Write-Ok 'Already present (use -Force to re-download)'
} else {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("retronest-ppsspp-" + [Guid]::NewGuid())
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        $zip = Join-Path $tmp 'PPSSPP.zip'
        Invoke-WebRequest -Uri 'https://buildbot.libretro.com/assets/system/PPSSPP.zip' -OutFile $zip -UseBasicParsing
        $ex = Join-Path $tmp 'x'
        Expand-Archive -Path $zip -DestinationPath $ex -Force
        $src = Join-Path $ex 'PPSSPP'
        if (-not (Test-Path $src)) { throw 'PPSSPP/ folder missing in the asset pack.' }
        if (Test-Path $PspDir) { Remove-Item $PspDir -Recurse -Force }
        New-Item -ItemType Directory -Path $PspDir -Force | Out-Null
        Copy-Item (Join-Path $src 'flash0') $PspDir -Recurse -Force
        foreach ($pattern in 'compat.ini', 'compatvr.ini', 'font_atlas.*', 'asciifont_atlas.*', 'LICENSE.TXT') {
            Copy-Item (Join-Path $src $pattern) $PspDir -Force
        }
        Write-Ok "Saved $PspDir"
    } catch {
        Write-Fail $_.Exception.Message
        $failed += 'ppsspp-assets'
    } finally {
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if ($failed.Count -gt 0) { throw "Failed to fetch: $($failed -join ', ')" }
Write-Host "`nAll cores present in $OutDir" -ForegroundColor Green
