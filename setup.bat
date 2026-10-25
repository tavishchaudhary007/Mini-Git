@echo off
rem One-command setup: build Mini-Git, configure PATH, then create a demo repo.
rem Usage: double-click, or run setup.bat from the minigit folder.
cd /d "%~dp0"

echo [1/3] Building Mini-Git...
set SRCS=
for %%f in (src\*.cpp) do call set SRCS=%%SRCS%% %%f
g++ -std=c++17 -Iinclude %SRCS% -o minigit.exe
if errorlevel 1 (
  echo Build failed. Is g++ installed and on PATH?
  exit /b 1
)

echo.
echo [2/3] Adding Mini-Git to User PATH...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$dir = '%~dp0'.TrimEnd('\'); $u = [Environment]::GetEnvironmentVariable('Path', 'User'); if (-not $u) { $u = '' }; $parts = $u -split ';' | Where-Object { $_ -ne '' }; if ($parts -notcontains $dir) { $new = ($parts + $dir) -join ';'; try { [Environment]::SetEnvironmentVariable('Path', $new, 'User'); Write-Host '  [+] Added' $dir 'to User PATH.' } catch { Write-Host '  [!] Could not update User PATH automatically.' } } else { Write-Host '  [*] Mini-Git is already in User PATH.' }"
set "PATH=%PATH%;%~dp0"

echo.
echo [3/3] Creating demo repository...
set MG=%~dp0minigit.exe

if exist ..\minigit-demo rmdir /s /q ..\minigit-demo 2>nul
mkdir ..\minigit-demo 2>nul
if exist ..\minigit-demo (
  cd ..\minigit-demo
  echo hello> a.txt
  "%MG%" init
  "%MG%" add a.txt
  "%MG%" commit -m "first commit"
  "%MG%" log --oneline
  echo.
  echo Demo repo created at: %cd%
)

echo.
echo ========================================================
echo Setup complete!
echo.
echo Mini-Git is configured on your PATH.
echo Open a new terminal window and run 'minigit' anywhere:
echo   minigit init
echo   minigit status
echo ========================================================
