@echo off
rem Compila d2dview.exe (solo renderer Direct2D) con MinGW GCC, CON CONSOLE per debug.
setlocal
set GCC="C:\Users\pr30565\Nim_code\mingw64\bin\gcc.exe"
set WINDRES="C:\Users\pr30565\Nim_code\mingw64\bin\windres.exe"

echo Compilazione risorsa icona...
%WINDRES% -i icon.rc -o icon.o
if errorlevel 1 goto :fail

echo Copia di litedocx.dll e liblitedocx.a...
copy /y ".\litedocx\litedocx.dll" . >nul
copy /y ".\litedocx\liblitedocx.a" . >nul

rem NOTA: niente renderer_gdi.c: il viewer usa solo Direct2D.
rem Build unity: maind2dv.c include tutti i moduli viewer_includes/*.c
rem RIMOSSO -mwindows per vedere console log
rem AGGIUNTO -DCONSOLE_LOGGING=1 per abilitare log viewer
rem AGGIUNTO -DENABLE_FILE_LOGGING=1 per log su file
%GCC% -Wall -O0 -g -ffunction-sections -fdata-sections -Wl,--gc-sections -I. -Ifunc_gdi -Iviewer_includes -Ilitedocx/include -DCONSOLE_LOGGING=1 -DENABLE_FILE_LOGGING=1 -o d2dview_debug.exe maind2dv.c func_gdi\layout.c func_gdi\layout_cache.c func_gdi\renderer_d2d.c func_gdi\text_utf8.c func_gdi\document.c icon.o -L. -llitedocx -lgdi32 -luser32 -lkernel32 -lcomdlg32 -lole32 -lwindowscodecs -lmsimg32 -ld2d1 -ldwrite -lcomctl32
if errorlevel 1 goto :fail

echo.
echo Build DEBUG OK: d2dview_debug.exe
exit /b 0

:fail
echo.
echo Build FALLITA.
exit /b 1