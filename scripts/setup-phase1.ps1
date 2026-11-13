<#
.SYNOPSIS
    One-time Phase 1 setup for RetroNest on Windows.

.DESCRIPTION
    - Writes local.properties (Android SDK path) and checks the pinned NDK / CMake versions
    - Downloads the official libretro.h into corehost/src/main/cpp/
    - Downloads the FCEUmm arm64 core into corehost/libs/arm64-v8a/libfceumm_libretro_android.so
    - Initializes a git repository if none exists

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\setup-phase1.ps1
    powershell -ExecutionPolicy Bypass -File scripts\setup-phase1.ps1 -Force   # re-download everything
#>
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$NdkVersion   = '30.0.15729638'
$CmakeVersion = '4.1.2'
$LibretroHeaderUrl = 'https://raw.githubusercontent.com/libretro/libretro-common/master/include/libretro.h'
$CoreZipUrl        = 'https://buildbot.libretro.com/nightly/android/latest/arm64-v8a/fceumm_libretro_android.so.zip'
$CoreTarget        = Join-Path $Root 'corehost\libs\arm64-v8a\libfceumm_libretro_android.so'

function Write-Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "    [OK]   $msg" -ForegroundColor Green }
function Write-Warn2($msg){ Write-Host "    [WARN] $msg" -ForegroundColor Yellow }

# ---------------------------------------------------------------- 1. Android SDK
Write-Step 'Android SDK'
# @(...) keeps this an array even with a single match (otherwise [0] returns the first character).
$sdkCandidates = @(@($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, (Join-Path $env:LOCALAPPDATA 'Android\Sdk')) |
    Where-Object { $_ -and (Test-Path $_) })
if (-not $sdkCandidates) {
    throw 'Android SDK not found. Set ANDROID_HOME or install the SDK via Android Studio.'
}
$Sdk = (Resolve-Path $sdkCandidates[0]).Path
Write-Ok "SDK: $Sdk"

$escaped = $Sdk -replace '\\', '\\' -replace ':', '\:'
Set-Content -Path (Join-Path $Root 'local.properties') -Value "sdk.dir=$escaped" -Encoding ASCII
Write-Ok 'local.properties written'

$ndkDir = Join-Path $Sdk "ndk\$NdkVersion"
if (Test-Path $ndkDir) { Write-Ok "NDK $NdkVersion" }
else {
    $have = (Get-ChildItem (Join-Path $Sdk 'ndk') -Directory -ErrorAction SilentlyContinue).Name -join ', '
    Write-Warn2 "NDK $NdkVersion missing (installed: $have). Update ndkVersion in corehost/build.gradle.kts or install it via SDK Manager."
}

$cmakeDir = Join-Path $Sdk "cmake\$CmakeVersion"
if (Test-Path $cmakeDir) { Write-Ok "CMake $CmakeVersion" }
else {
    $have = (Get-ChildItem (Join-Path $Sdk 'cmake') -Directory -ErrorAction SilentlyContinue).Name -join ', '
    Write-Warn2 "CMake $CmakeVersion missing (installed: $have). Update cmake version in corehost/build.gradle.kts or install it via SDK Manager."
}

# ---------------------------------------------------------------- 2. JDK
Write-Step 'JDK'
$studioJbr = 'C:\Program Files\Android\Android Studio\jbr'
if ($env:JAVA_HOME -and (Test-Path (Join-Path $env:JAVA_HOME 'bin\java.exe'))) {
    Write-Ok "JAVA_HOME: $env:JAVA_HOME"
} elseif (Test-Path (Join-Path $studioJbr 'bin\java.exe')) {
    Write-Warn2 "JAVA_HOME not set. Android Studio's JDK exists; to use gradlew from a terminal run:"
    Write-Host  "           `$env:JAVA_HOME = '$studioJbr'"
} else {
    Write-Warn2 'No JDK found. Install Android Studio or a JDK 17+.'
}

# ---------------------------------------------------------------- 3. libretro.h
Write-Step 'libretro.h (official header)'
$header = Join-Path $Root 'corehost\src\main\cpp\libretro.h'
if ((Test-Path $header) -and -not $Force) {
    Write-Ok 'Already present (use -Force to re-download)'
} else {
    Invoke-WebRequest -Uri $LibretroHeaderUrl -OutFile $header -UseBasicParsing
    if (-not (Select-String -Path $header -Pattern 'RETRO_API_VERSION' -Quiet)) {
        throw 'Downloaded libretro.h looks invalid (RETRO_API_VERSION not found).'
    }
    Write-Ok "Downloaded to $header"
}

# ---------------------------------------------------------------- 4. FCEUmm core
Write-Step 'FCEUmm NES core (arm64-v8a)'
if ((Test-Path $CoreTarget) -and -not $Force) {
    Write-Ok 'Already present (use -Force to re-download)'
} else {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("retronest-core-" + [Guid]::NewGuid())
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        $zip = Join-Path $tmp 'core.zip'
        Invoke-WebRequest -Uri $CoreZipUrl -OutFile $zip -UseBasicParsing
        Expand-Archive -Path $zip -DestinationPath $tmp -Force
        $so = Get-ChildItem $tmp -Filter '*.so' -Recurse | Select-Object -First 1
        if (-not $so) { throw 'No .so found inside the core archive.' }
        New-Item -ItemType Directory -Path (Split-Path $CoreTarget) -Force | Out-Null
        Move-Item -Path $so.FullName -Destination $CoreTarget -Force
        Write-Ok ("Saved {0} ({1:N0} KB)" -f $CoreTarget, ((Get-Item $CoreTarget).Length / 1KB))
    } finally {
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------- 4b. Remaining bundled cores
Write-Step 'Other bundled cores (cores_catalog.json)'
$fetch = Join-Path $PSScriptRoot 'fetch-cores.ps1'
if ($Force) { & $fetch -Force } else { & $fetch }

# ---------------------------------------------------------------- 5. Git
Write-Step 'Git'
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Warn2 'git not on PATH; skipping.'
} elseif (Test-Path (Join-Path $Root '.git')) {
    Write-Ok 'Repository already initialized'
} else {
    git init -b main | Out-Null
    Write-Ok 'Initialized empty repository on branch main (nothing committed yet)'
}

Write-Step 'Done'
Write-Host '    Next: open the folder in Android Studio (or run .\gradlew.bat assembleDebug),'
Write-Host '          then: powershell -ExecutionPolicy Bypass -File scripts\verify-phase1.ps1 -RomPath <file.nes>'
