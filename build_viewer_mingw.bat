@echo off
rem Compila docxview_new.exe con MinGW GCC (mantiene la console aperta)
setlocal
set GCC="C:\Users\pr30565\Nim_code\mingw64\bin\gcc.exe"
set GXX="C:\Users\pr30565\Nim_code\mingw64\bin\g++.exe"

echo Copia di litedocx.dll e liblitedocx.a...
copy /y ".\litedocx\litedocx.dll" . >nul
copy /y ".\litedocx\liblitedocx.a" . >nul

echo Compilazione con MinGW GCC (console + GUI)...
%GCC% -Wall -O2 -I. -mconsole -o docxview_new.exe docxview_new.c -L. -llitedocx -lgdi32 -luser32 -lkernel32 -lcomdlg32 -lole32 -lwindowscodecs -lmsimg32
if errorlevel 1 goto :fail

echo.
echo Build OK: docxview_new.exe (con console)
exit /b 0

:fail
echo.
echo Build FALLITA.
exit /b 1