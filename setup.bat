@echo off
rem One-command setup: build Mini-Git, then create a demo repo with a first commit.
rem Usage: double-click, or run setup.bat from the minigit folder.
cd /d "%~dp0"

echo Building...
set SRCS=
for %%f in (src\*.cpp) do call set SRCS=%%SRCS%% %%f
g++ -std=c++17 -Iinclude %SRCS% -o minigit.exe
if errorlevel 1 (
  echo Build failed. Is g++ installed and on PATH?
  exit /b 1
)
set MG=%cd%\minigit.exe

if exist ..\minigit-demo rmdir /s /q ..\minigit-demo
mkdir ..\minigit-demo
cd ..\minigit-demo

echo hello> a.txt
"%MG%" init
"%MG%" add a.txt
"%MG%" commit -m "first commit"
"%MG%" log --oneline

echo.
echo Done. Demo repo: %cd%
echo Run commands there with: "%MG%" ^<command^>
