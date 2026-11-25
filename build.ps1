# build.ps1 — Compiles CFG-enabled TransparentTaskbar.dll (v3) and tb_loader.exe
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$srcDir = Join-Path $root 'src'
$binDir = Join-Path $root 'bin'
$objDir = Join-Path $root 'obj'

New-Item -ItemType Directory -Force -Path $binDir, $objDir | Out-Null

# Clean up any previously unlocked stale DLLs; rename currently locked DLL if explorer.exe holds it
Get-ChildItem $objDir -Filter 'stale_*.dll' -ErrorAction SilentlyContinue | ForEach-Object {
    Remove-Item -Force -ErrorAction SilentlyContinue $_.FullName
}
$dllOut = Join-Path $binDir 'TransparentTaskbar.dll'
if (Test-Path $dllOut) {
    try {
        [System.IO.File]::OpenWrite($dllOut).Close()
    } catch {
        $stale = Join-Path $objDir ("stale_" + [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() + ".dll")
        Write-Host "Bilgi: Eski TransparentTaskbar.dll explorer.exe tarafindan kullaniliyor, gecici olarak tasiniyor..." -ForegroundColor Yellow
        Move-Item -Force -Path $dllOut -Destination $stale
    }
}

# Locate vcvars64.bat
$vcvarsCandidates = @(
    'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
)
$vcvars = $vcvarsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) {
    $vcvars = Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio' -Filter 'vcvars64.bat' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $vcvars) {
    throw 'Visual Studio vcvars64.bat bulunamadi!'
}

Write-Host "[1/3] MSVC ortami yukleniyor: $vcvars" -ForegroundColor Cyan

$exeOut = Join-Path $binDir 'tb_loader.exe'

$cmd = @"
call "$vcvars" >nul
if errorlevel 1 exit /b 1

echo [2/3] TransparentTaskbar.dll (v6, CFG-enabled) derleniyor...
cl.exe /nologo /std:c++20 /O2 /Os /GL /MT /EHsc /guard:cf /W3 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
    /LD /Fo"$objDir\\" /Fe"$dllOut" "$srcDir\tap_dll.cpp" ^
    /link /LTCG /OPT:REF /OPT:ICF /GUARD:CF /DEF:"$srcDir\tap_dll.def" ^
    user32.lib gdi32.lib dwmapi.lib shell32.lib ole32.lib oleaut32.lib comctl32.lib runtimeobject.lib windowsapp.lib
if errorlevel 1 exit /b 1

echo [3/3] tb_loader.exe (v6) derleniyor...
cl.exe /nologo /std:c++20 /O2 /Os /GL /MT /EHsc /W3 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
    /Fo"$objDir\\" /Fe"$exeOut" "$srcDir\loader.cpp" ^
    /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS ^
    user32.lib gdi32.lib
if errorlevel 1 exit /b 1
"@

$batPath = Join-Path $objDir 'build_run.bat'
Set-Content -Path $batPath -Value $cmd -Encoding ASCII

& cmd.exe /c "`"$batPath`""
if ($LASTEXITCODE -ne 0) {
    throw "Derleme basarisiz (exit code: $LASTEXITCODE)"
}

Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $binDir 'TransparentTaskbar.lib'), (Join-Path $binDir 'TransparentTaskbar.exp')

Write-Host "`nDerleme basarili!" -ForegroundColor Green
Get-ChildItem $binDir | Select-Object Name, @{N='SizeKB';E={[math]::Round($_.Length/1KB,1)}} | Format-Table -AutoSize
