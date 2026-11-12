@echo off
rem Compila docxview_new.exe con MinGW GCC (no console, -mwindows)
setlocal
set GCC="C:\Users\pr30565\Nim_code\mingw64\bin\gcc.exe"
set GXX="C:\Users\pr30565\Nim_code\mingw64\bin\g++.exe"

set WINDRES="C:\Users\pr30565\Nim_code\mingw64\bin\windres.exe"

echo Compilazione risorsa icona...
%WINDRES% -i icon.rc -o icon.o
if errorlevel 1 goto :fail

echo Copia di litedocx.dll e liblitedocx.a...
copy /y ".\litedocx\litedocx.dll" . >nul
copy /y ".\litedocx\liblitedocx.a" . >nul

echo Compilazione con MinGW GCC (GUI, no console)...
%GCC% -Wall -O2 -mwindows -I. -Ifunc_gdi -Ilitedocx/include -o docxview_new.exe docxview_new.c func_gdi\layout.c func_gdi\renderer_gdi.c func_gdi\renderer_d2d.c func_gdi\text_utf8.c func_gdi\document.c icon.o -L. -llitedocx -lgdi32 -luser32 -lkernel32 -lcomdlg32 -lole32 -lwindowscodecs -lmsimg32 -ld2d1 -ldwrite
if errorlevel 1 goto :fail

echo.
echo Build OK: docxview_new.exe (-mwindows per log in console)
exit /b 0

:fail
echo.
echo Build FALLITA.
exit /b 1