@echo off
setlocal

rem Build DiskStressServer.exe (self-drawn GUI server) with MSVC cl.exe.
rem Run from "x64 Native Tools Command Prompt for VS" or any shell with cl.exe on PATH.

where cl.exe >nul 2>nul
if errorlevel 1 (
  echo [ERROR] cl.exe not found.
  echo Open "x64 Native Tools Command Prompt for VS 2022" and run this script again.
  exit /b 1
)

set CFLAGS=/nologo /std:c++17 /utf-8 /EHsc /O2 /W3 /GS /Gy /MT /D_UNICODE /DUNICODE /DWIN32 /D_WINDOWS
set LFLAGS=/SUBSYSTEM:WINDOWS /MACHINE:X64 /OPT:REF /OPT:ICF

if not exist out mkdir out

echo [1/1] Building DiskStressServer.exe ...
cl %CFLAGS% /Fo:out\ /Fd:out\server.pdb src\server_gui.cpp /link %LFLAGS% /OUT:out\DiskStressServer.exe
if errorlevel 1 (
  echo [ERROR] build failed
  exit /b 1
)

echo.
echo [OK] Output: out\DiskStressServer.exe
dir /b out\*.exe
endlocal
