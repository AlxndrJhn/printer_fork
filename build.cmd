@echo off
rem Builds printer_fork.exe with MinGW-w64 (windres + g++ on PATH).
rem Our manifest is compiled as build\default-manifest.o; -B<abs>/build/ makes the linker use it
rem instead of MinGW's own default manifest (two manifests break the link).
cd /d "%~dp0"
if not exist build mkdir build
windres manifest.rc -O coff -o build/default-manifest.o || exit /b 1
windres printer_fork.rc -O coff -o build/printer_fork.res || exit /b 1
g++ "-B%~dp0build/" -std=c++17 -Os -s -static -municode -mwindows -Wall printer_fork.cpp build/printer_fork.res -o printer_fork.exe -lcomctl32 -lole32 -luuid -lshell32 || exit /b 1
echo Built printer_fork.exe
