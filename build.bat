@echo off
setlocal
cd /d "%~dp0"

if not exist "tools\ww2ogg.exe" goto :notools
if not exist "tools\hpatchz.exe" goto :notools

echo [1/4] Compiling encoder resources...
windres -i payload_enc.rc -o payload_enc.o
if errorlevel 1 goto :fail

echo [2/4] Building WemEnc.exe...
gcc -O2 -s -static -DENCODER -o WemEnc.exe wemtool.c payload_enc.o
if errorlevel 1 goto :fail

echo [3/4] Compiling decoder resources...
windres -i payload_dec.rc -o payload_dec.o
if errorlevel 1 goto :fail

echo [4/4] Building WemDec.exe...
gcc -O2 -s -static -DDECODER -o WemDec.exe wemtool.c payload_dec.o
if errorlevel 1 goto :fail

del /q payload_enc.o payload_dec.o 2>nul>nul

echo.
echo Build complete:
echo   WemEnc.exe  - encoder
echo   WemDec.exe  - decoder
exit /b 0

:notools
echo ERROR: tools folder is missing. Run this build script from the c_src folder.
exit /b 1

:fail
echo.
echo BUILD FAILED.
exit /b 1
