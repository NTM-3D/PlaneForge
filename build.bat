@echo off
setlocal

set "VER=0.1"
set "CC=gcc"
set "RC=windres"
where gcc >nul 2>nul
if errorlevel 1 (
    set "CC=C:\Users\NTM\AppData\Local\Temp\opencode\w64devkit\w64devkit\bin\gcc.exe"
    set "RC=C:\Users\NTM\AppData\Local\Temp\opencode\w64devkit\w64devkit\bin\windres.exe"
)

echo Building PlaneForge v%VER% with "%CC%" ...
"%RC%" src\version.rc -O coff -o src\version_rc.o
if errorlevel 1 (
    echo Resource build failed.
    exit /b 1
)
"%CC%" -O2 -std=c11 -Wall -Wextra -o PlaneForge_v%VER%.exe ^
    src\annexb.c src\ofmd.c src\ofs.c src\mvc.c src\progress.c src\main.c ^
    src\version_rc.o
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)
echo Built: PlaneForge_v%VER%.exe
endlocal
