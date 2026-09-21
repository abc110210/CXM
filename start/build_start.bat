@echo off
setlocal

rem Build DiskStressStart.exe (silent worker + Windows service) with MSVC cl.exe.
rem Run from "x64 Native Tools Command Prompt for VS" or any shell where cl.exe is on PATH.

where cl.exe >nul 2>nul
if errorlevel 1 (
  echo [ERROR] cl.exe not found.
  echo Open "x64 Native Tools Command Prompt for VS 2022" and run this script again.
  exit /b 1
)

set SRC=src\shared.cpp src\report.cpp src\worker.cpp src\net.cpp
set CFLAGS=/nologo /std:c++17 /utf-8 /EHsc /O2 /W3 /GS /Gy /MT /D_UNICODE /DUNICODE /DWIN32 /D_WINDOWS
set LFLAGS=/SUBSYSTEM:WINDOWS /MACHINE:X64 /OPT:REF /OPT:ICF

if not exist out mkdir out

echo [1/1] Building DiskStressStart.exe ...
cl %CFLAGS% /Fo:out\ /Fd:out\start.pdb %SRC% src\main.cpp /link %LFLAGS% /OUT:out\DiskStressStart.exe
if errorlevel 1 (
  echo [ERROR] build failed
  exit /b 1
)

copy /Y README.md out\README.md >nul

echo.
echo [OK] Output: out\DiskStressStart.exe
dir /b out\*.exe
endlocal
