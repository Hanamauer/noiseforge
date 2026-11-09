# ---------------------------------------------------------------------------
#  build.ps1 -- NOISEFORGE
#
#  g++ only, no CMake.  Requires MSYS2 mingw-w64 (g++) and a copy of the
#  Steinberg VST3 SDK:
#
#      git clone --recursive https://github.com/steinbergmedia/vst3sdk
#
#  Usage:
#      $env:VST3_SDK = 'C:\dev\vst3sdk'
#      powershell -ExecutionPolicy Bypass -File build.ps1
#
#      powershell -ExecutionPolicy Bypass -File build.ps1 -Sdk C:\dev\vst3sdk
#      powershell -ExecutionPolicy Bypass -File build.ps1 -ToolsOnly
#
#  Outputs
#      build\NoiseForge.vst3                              flat binary (for the tools)
#      dist\NoiseForge.vst3\Contents\x86_64-win\...       VST3 bundle to install
#      build\selftest.exe, build\host_test.exe, build\gui_render.exe
# ---------------------------------------------------------------------------
param(
    [string]$Sdk = $env:VST3_SDK,
    [switch]$ToolsOnly,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path $PSScriptRoot).Path

# ---- toolchain -------------------------------------------------------------
$Gxx = $null
$cmd = Get-Command g++ -ErrorAction SilentlyContinue
if ($cmd) { $Gxx = $cmd.Source }
if (-not $Gxx -and (Test-Path 'C:\msys64\mingw64\bin\g++.exe')) {
    $Gxx = 'C:\msys64\mingw64\bin\g++.exe'
    $env:PATH = 'C:\msys64\mingw64\bin;' + $env:PATH
}
if (-not $Gxx) { throw 'g++ not found: install MSYS2 (mingw-w64) or put it on PATH.' }

if (-not $Sdk) {
    throw 'VST3 SDK not found. Pass -Sdk <path\to\vst3sdk> or set $env:VST3_SDK.'
}
$Sdk = (Resolve-Path $Sdk).Path
if (-not (Test-Path (Join-Path $Sdk 'public.sdk'))) {
    throw "not a VST3 SDK (no public.sdk): $Sdk"
}

$Bin = 'build'
$Obj = 'build\obj'
New-Item -ItemType Directory -Force -Path $Obj, $Bin | Out-Null

# Always run from the project root: relative source paths are what keep this
# working on machines whose project path has non-ASCII characters.
Push-Location $Root
try {

$sdkSrc = @(
  'base\source\baseiids.cpp'
  'base\source\fbuffer.cpp'
  'base\source\fdebug.cpp'
  'base\source\fdynlib.cpp'
  'base\source\fobject.cpp'
  'base\source\fstreamer.cpp'
  'base\source\fstring.cpp'
  'base\source\timer.cpp'
  'base\source\updatehandler.cpp'
  'base\thread\source\flock.cpp'
  'base\thread\source\fcondition.cpp'
  'pluginterfaces\base\conststringtable.cpp'
  'pluginterfaces\base\coreiids.cpp'
  'pluginterfaces\base\funknown.cpp'
  'pluginterfaces\base\ustring.cpp'
  'public.sdk\source\common\commoniids.cpp'
  'public.sdk\source\common\commonstringconvert.cpp'
  'public.sdk\source\common\memorystream.cpp'
  'public.sdk\source\common\pluginview.cpp'
  'public.sdk\source\common\readfile.cpp'
  'public.sdk\source\vst\vstaudioeffect.cpp'
  'public.sdk\source\vst\vstbus.cpp'
  'public.sdk\source\vst\vstcomponent.cpp'
  'public.sdk\source\vst\vstcomponentbase.cpp'
  'public.sdk\source\vst\vstinitiids.cpp'
  'public.sdk\source\vst\vstparameters.cpp'
  'public.sdk\source\vst\vstsinglecomponenteffect.cpp'
  'public.sdk\source\main\dllmain.cpp'
  'public.sdk\source\main\moduleinit.cpp'
  'public.sdk\source\main\pluginfactory.cpp'
)

$mySrc = @(
  'src\NoiseForgeCore.cpp'
  'vst3\NoiseForgeVST3.cpp'
  'vst3\NoiseForgeEditor.cpp'
)

$myFlags  = @('-O2','-std=c++17','-Wall','-Wextra','-fno-fast-math',
              '-DRELEASE=1','-DNDEBUG','-I','src','-I','vst3','-isystem',$Sdk)
$sdkFlags = @('-O2','-std=c++17','-w','-fno-fast-math',
              '-DRELEASE=1','-DNDEBUG','-isystem',$Sdk)

$libs = @('-lgdiplus','-lgdi32','-luser32','-lole32','-luuid','-loleaut32',
          '-lshell32','-lcomdlg32','-ladvapi32','-lwinhttp','-lws2_32')

function Compile([string]$src, [string]$obj, [string[]]$flags) {
    if ((Test-Path $obj) -and
        ((Get-Item $obj).LastWriteTime -gt (Get-Item $src).LastWriteTime)) {
        return $obj
    }
    & $Gxx @flags -c -o $obj $src
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $src" }
    return $obj
}

$sw = [Diagnostics.Stopwatch]::StartNew()

# ---- engine self-test ------------------------------------------------------
Write-Host ''
Write-Host '--- engine self-test ---'
$coreObj = Compile 'src\NoiseForgeCore.cpp' "$Obj\core.o" $myFlags
$testObj = Compile 'tools\selftest.cpp' "$Obj\selftest.o" $myFlags
& $Gxx -O2 -static -o "$Bin\selftest.exe" $testObj $coreObj
if ($LASTEXITCODE -ne 0) { throw 'link failed: selftest.exe' }
Write-Host ("    built selftest.exe ({0:N0} bytes)" -f (Get-Item "$Bin\selftest.exe").Length)

if ($ToolsOnly) {
    Write-Host ("done in {0:N1}s" -f $sw.Elapsed.TotalSeconds)
    return
}

# ---- plugin ----------------------------------------------------------------
Write-Host ''
Write-Host '--- plugin ---'
$objs = @()
foreach ($s in $mySrc) {
    $name = ($s -replace '[\\/]', '_') -replace '\.cpp$', '.o'
    $objs += (Compile $s "$Obj\$name" $myFlags)
}
$i = 0
foreach ($s in $sdkSrc) {
    $i++
    $p = Join-Path $Sdk $s
    if (-not (Test-Path $p)) { throw "missing SDK source: $s" }
    $objs += (Compile $p "$Obj\sdk$i.o" $sdkFlags)
}

$flat = "$Bin\NoiseForge.vst3"
# -static pulls in libstdc++ / libgcc / winpthread statically: a plugin that
# imports libwinpthread-1.dll will not load in a host on a clean machine.
& $Gxx -shared -o $flat @objs -static @libs
if ($LASTEXITCODE -ne 0) { throw 'link failed: NoiseForge.vst3' }

$bundle = 'dist\NoiseForge.vst3\Contents\x86_64-win'
New-Item -ItemType Directory -Force -Path $bundle | Out-Null
Copy-Item -Force $flat (Join-Path $bundle 'NoiseForge.vst3')

Write-Host ("    built NoiseForge.vst3 ({0:N0} bytes)" -f (Get-Item $flat).Length)
Write-Host ("    bundle: {0}" -f (Resolve-Path 'dist\NoiseForge.vst3').Path)

# ---- host simulation -------------------------------------------------------
Write-Host ''
Write-Host '--- host simulation ---'
$hostSdk = @(
  'pluginterfaces\base\coreiids.cpp'
  'pluginterfaces\base\funknown.cpp'
  'public.sdk\source\vst\vstinitiids.cpp'
  'public.sdk\source\common\memorystream.cpp'
)
$hostObjs = @($coreObj)
$i = 0
foreach ($s in $hostSdk) {
    $i++
    $hostObjs += (Compile (Join-Path $Sdk $s) "$Obj\host$i.o" $sdkFlags)
}
$hostObj = Compile 'tools\host_test.cpp' "$Obj\host_test.o" $myFlags
& $Gxx -O2 -static -o "$Bin\host_test.exe" $hostObj @hostObjs -lgdi32 -luser32 -lole32 -luuid
if ($LASTEXITCODE -ne 0) { throw 'link failed: host_test.exe' }
& "$Bin\host_test.exe" $flat
if ($LASTEXITCODE -ne 0) { throw 'host simulation failed' }

# ---- panel render ----------------------------------------------------------
Write-Host ''
Write-Host '--- GUI render ---'
$guiObj = Compile 'tools\gui_render.cpp' "$Obj\gui_render.o" $myFlags
& $Gxx -O2 -static -o "$Bin\gui_render.exe" $guiObj @hostObjs -lgdi32 -luser32 -lole32 -luuid
if ($LASTEXITCODE -ne 0) { throw 'link failed: gui_render.exe' }
& "$Bin\gui_render.exe" $flat "$Bin\ui.png" --live --refresh
if ($LASTEXITCODE -ne 0) { throw 'GUI render failed' }
Write-Host ("    panel: {0}" -f (Resolve-Path "$Bin\ui.png").Path)

if (-not $SkipTests) {
    Write-Host ''
    Write-Host '--- self-test ---'
    & "$Bin\selftest.exe"
    if ($LASTEXITCODE -ne 0) { throw 'engine self-test failed' }
}

Write-Host ("done in {0:N1}s" -f $sw.Elapsed.TotalSeconds)

} finally {
    Pop-Location
}

exit 0

