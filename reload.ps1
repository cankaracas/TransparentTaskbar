# reload.ps1 — Re-applies transparency, full-screen work area, and click-through regions (e.g. after manual explorer restart)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$loader = Join-Path $root 'bin\tb_loader.exe'

if (-not (Test-Path $loader)) {
    throw 'bin\tb_loader.exe bulunamadi. Once build.ps1 calistirin.'
}

$proc = Start-Process -FilePath $loader -ArgumentList '--refresh' -WorkingDirectory (Join-Path $root 'bin') -Wait -PassThru
if ($proc.ExitCode -eq 0) {
    Write-Host 'TransparentTaskbar yenilendi.' -ForegroundColor Green
} else {
    Write-Warning "tb_loader.exe cikis kodu: $($proc.ExitCode) (0x$($proc.ExitCode.ToString('X8')))"
}
