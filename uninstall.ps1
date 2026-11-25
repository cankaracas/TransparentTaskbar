# uninstall.ps1 — Instantly restores default Windows 11 taskbar and removes startup registration
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$loader = Join-Path $root 'bin\tb_loader.exe'
$taskName = 'TransparentTaskbarAutoLoad'

Write-Host '[1/2] Gorev cubugu varsayilan Windows haline donduruluyor...' -ForegroundColor Cyan
if (Test-Path $loader) {
    $proc = Start-Process -FilePath $loader -ArgumentList '--restore' -WorkingDirectory (Join-Path $root 'bin') -Wait -PassThru
    Write-Host "Geri yukleme sinyali gonderildi (kod: $($proc.ExitCode))." -ForegroundColor Green
}

Write-Host '[2/2] Otomatik baslatma kayitlari temizleniyor...' -ForegroundColor Cyan
cmd.exe /c "schtasks /Delete /TN `"$taskName`" /F >nul 2>&1"
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name $taskName -ErrorAction SilentlyContinue

Write-Host "`nTamamlandi! Gorev cubugunuz eski haline donduruldu ve acilis kaydi silindi." -ForegroundColor Green
Write-Host '(Eger tam ekran pencereler hala alttan tasiyorsa pencereyi kucultup buyutmeniz yeterlidir.)' -ForegroundColor DarkGray
