@echo off
setlocal

for /f "tokens=3" %%i in ('findstr /B /C:"#define PF_VER_MAJOR" src\version.h') do set "MAJOR=%%i"
for /f "tokens=3" %%i in ('findstr /B /C:"#define PF_VER_MINOR" src\version.h') do set "MINOR=%%i"
set "VER=%MAJOR%.%MINOR%"

if not defined CC set "CC=gcc"
if not defined RC set "RC=windres"
where %CC% >nul 2>nul
if errorlevel 1 (
    echo Error: %CC% was not found on PATH. Install a C11 toolchain such as w64devkit or MinGW-w64,
    echo or set the CC and RC environment variables to your compiler and windres.
    exit /b 1
)
where %RC% >nul 2>nul
if errorlevel 1 (
    echo Error: %RC% was not found on PATH.
    exit /b 1
)

echo Building PlaneForge v%VER% with "%CC%" ...
"%RC%" src\version.rc -O coff -o src\version_rc.o
if errorlevel 1 (
    echo Resource build failed.
    exit /b 1
)
"%CC%" -O2 -std=c11 -Wall -Wextra -o PlaneForge_v%VER%.exe ^
    src\annexb.c src\ofmd.c src\ofs.c src\mvc.c src\progress.c src\jval.c ^
    src\pjson.c src\main.c src\version_rc.o
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)
echo Built: PlaneForge_v%VER%.exe
endlocal
