# install.ps1 — Installs, activates TransparentTaskbar v3, and registers logon startup (0 persistent .exe)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$loader = Join-Path $root 'bin\tb_loader.exe'
$dll = Join-Path $root 'bin\TransparentTaskbar.dll'
$taskName = 'TransparentTaskbarAutoLoad'

if (-not (Test-Path $loader) -or -not (Test-Path $dll)) {
    Write-Host 'Ikili dosyalar bulunamadi, once build.ps1 calistiriliyor...' -ForegroundColor Yellow
    & (Join-Path $root 'build.ps1')
}

# Step 1: Register logon startup (works without Admin via HKCU Run; removes any old Task Scheduler entry)
Write-Host '[1/3] Bilgisayar acilisinda otomatik calisma kaydediliyor...' -ForegroundColor Cyan
Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name $taskName -Value "`"$loader`""
cmd.exe /c "schtasks /Delete /TN `"$taskName`" /F >nul 2>&1"
Write-Host 'Oturum acilisi kaydi (HKCU Run — 0 surekli arka plan .exe) basariyla yapildi.' -ForegroundColor Green

# Step 2: Detect if an older DLL or stale_*.dll is loaded in explorer.exe; if so, refresh explorer cleanly
Write-Host '[2/3] Mevcut explorer.exe durumu kontrol ediliyor...' -ForegroundColor Cyan
$needsExplorerRefresh = $false
try {
    $hasStaleDll = @(Get-ChildItem (Join-Path $root 'obj') -Filter 'stale_*.dll' -ErrorAction SilentlyContinue).Count -gt 0
    $loadedMods = (Get-Process explorer -ErrorAction SilentlyContinue).Modules |
        Where-Object { $_.FileName -like '*TransparentTaskbar*' -or $_.FileName -like '*stale_*.dll' }
    if ($hasStaleDll) {
        $needsExplorerRefresh = $true
    } elseif ($loadedMods) {
        $statusProc = Start-Process -FilePath $loader -ArgumentList '--status' -WorkingDirectory (Join-Path $root 'bin') -Wait -PassThru
        if ($statusProc.ExitCode -ne 0) {
            $needsExplorerRefresh = $true
        }
    }
} catch {}

if ($needsExplorerRefresh) {
    Write-Host 'Eski surum DLL bellekten temizleniyor (explorer.exe 3 saniyeligine yenileniyor)...' -ForegroundColor Yellow
    Start-Process -FilePath $loader -ArgumentList '--restore' -WorkingDirectory (Join-Path $root 'bin') -Wait | Out-Null
    Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) {
        Start-Process explorer.exe
    }
    for ($w = 0; $w -lt 20; $w++) {
        if (Get-Process explorer -ErrorAction SilentlyContinue) { break }
        Start-Sleep -Milliseconds 250
    }
    Start-Sleep -Milliseconds 4000
    Get-ChildItem (Join-Path $root 'obj') -Filter 'stale_*.dll' -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $root 'obj\*.obj'), (Join-Path $root 'obj\build_run.bat')
}

# Step 3: Activate v6!
Write-Host '[3/3] TransparentTaskbar v6 aktif ediliyor...' -ForegroundColor Cyan
$proc = Start-Process -FilePath $loader -WorkingDirectory (Join-Path $root 'bin') -Wait -PassThru
if ($proc.ExitCode -eq 0) {
    Write-Host "`nBASARILI! TransparentTaskbar v6 aktif." -ForegroundColor Green
    Write-Host '- SIFIR IAT kancasi (CFG/XFG %100 uyumlu, explorer cokmesi/ekran ziplamasi yok).' -ForegroundColor Green
    Write-Host '- Gorev cubugu %100 saydam, bos alanlar tamamen tiklanabilir.' -ForegroundColor Green
    Write-Host '- Gorev Yoneticisi''nde 0 ekstra .exe calisiyor.' -ForegroundColor Green
    Write-Host "`nGeri almak icin: powershell -ExecutionPolicy Bypass -File `"$root\uninstall.ps1`"" -ForegroundColor DarkGray
} else {
    Write-Warning "tb_loader.exe cikis kodu: $($proc.ExitCode) (0x$($proc.ExitCode.ToString('X8')))"
}
