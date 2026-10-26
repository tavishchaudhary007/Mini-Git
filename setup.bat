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
echo [2/2] Adding Mini-Git to User PATH...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$dir = '%~dp0'.TrimEnd('\'); $u = [Environment]::GetEnvironmentVariable('Path', 'User'); if (-not $u) { $u = '' }; $parts = $u -split ';' | Where-Object { $_ -ne '' }; if ($parts -notcontains $dir) { $new = ($parts + $dir) -join ';'; try { [Environment]::SetEnvironmentVariable('Path', $new, 'User'); Write-Host '  [+] Added' $dir 'to User PATH.' } catch { Write-Host '  [!] Could not update User PATH automatically.' } } else { Write-Host '  [*] Mini-Git is already in User PATH.' }"
set "PATH=%PATH%;%~dp0"

echo.
echo ========================================================
echo Setup complete! Mini-Git is built and ready.
echo.
echo You can open any terminal and run commands anywhere:
echo   minigit init
echo   minigit status
echo   minigit ui
echo.
echo Tip: To use it immediately in this open PowerShell window
echo without restarting, run:
echo   $env:Path += ";%~dp0"
echo ========================================================
