@echo off
rem One-command setup: build Mini-Git and configure PATH.
rem Usage: double-click, or run setup.bat from the minigit folder.
cd /d "%~dp0"

echo [1/2] Building Mini-Git...
set SRCS=
for %%f in (src\*.cpp) do call set SRCS=%%SRCS%% %%f
g++ -std=c++17 -Iinclude %SRCS% -o minigit.exe -lws2_32
if errorlevel 1 (
  echo Build failed. Is g++ installed and on PATH?
  exit /b 1
)

echo.
echo [2/2] Configuring environment and PATH...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$dir = '%~dp0'.TrimEnd('\'); $u = [Environment]::GetEnvironmentVariable('Path', 'User'); if (-not $u) { $u = '' }; $parts = $u -split ';' | Where-Object { $_ -ne '' }; if ($parts -notcontains $dir) { $new = ($parts + $dir) -join ';'; try { [Environment]::SetEnvironmentVariable('Path', $new, 'User'); Write-Host '  [+] Added' $dir 'to User PATH.' } catch { Write-Host '  [!] Could not update User PATH automatically.' } } else { Write-Host '  [*] Mini-Git is already in User PATH.' }; $docs = [Environment]::GetFolderPath('MyDocuments'); foreach ($sub in @('WindowsPowerShell', 'PowerShell')) { try { $pdir = Join-Path $docs $sub; if (-not (Test-Path $pdir)) { New-Item -ItemType Directory -Path $pdir -Force | Out-Null }; $pf = Join-Path $pdir 'profile.ps1'; $line = 'if ($env:Path -notlike ''*' + $dir + '*'') { $env:Path += '';' + $dir + ''' }'; $curr = if (Test-Path $pf) { Get-Content $pf -Raw } else { '' }; if ($curr -notlike ('*' + $dir + '*')) { Add-Content -Path $pf -Value $line -Force; Write-Host '  [+] Configured PowerShell profile in' $sub '.'; } } catch {} }"
set "PATH=%PATH%;%~dp0"

echo.
echo ========================================================
echo Setup complete! Mini-Git is built and ready.
echo.
echo You can run commands anywhere (including VS Code terminals):
echo   minigit init
echo   minigit status
echo   minigit ui
echo.
echo Tip: To use it immediately in this open window without
echo restarting, run:
echo   $env:Path += ";%~dp0"
echo ========================================================
