@echo off
rem Compila una versione diagnostica (console) con timing abilitato.
rem 1) Ricostruisce litedocx.dll in Debug (D2D_DEBUG_LOGS=1 + TIMING_LOGS=1).
rem 2) Compila d2dview_timing.exe con console e timing.
setlocal
set GCC="C:\Users\pr30565\Nim_code\mingw64\bin\gcc.exe"
set WINDRES="C:\Users\pr30565\Nim_code\mingw64\bin\windres.exe"

echo [1/3] Rebuild litedocx.dll (Debug, timing)...
call ".\litedocx\build_litedocx.bat" Timing
if errorlevel 1 goto :fail

echo [2/3] Copia DLL/import lib...
copy /y ".\litedocx\litedocx.dll" . >nul
copy /y ".\litedocx\liblitedocx.a" . >nul

rem Build unity, con console (no -mwindows) e timing (senza icona)
%GCC% -Wall -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections -I. -Ifunc_gdi -Iviewer_includes -Ilitedocx/include -DD2D_DEBUG_LOGS=0 -DTIMING_LOGS=1 -o d2dview_timing.exe maind2dv.c miniz_zip\miniz.c func_gdi\layout.c func_gdi\layout_cache.c func_gdi\renderer_d2d.c func_gdi\text_utf8.c func_gdi\document.c -L. -llitedocx -lgdi32 -luser32 -lkernel32 -lcomdlg32 -lole32 -lwindowscodecs -lmsimg32 -ld2d1 -ldwrite -lcomctl32
if errorlevel 1 goto :fail

echo.
echo Build TIMING OK: d2dview_timing.exe
exit /b 0

:fail
echo.
echo Build TIMING FALLITA.
exit /b 1