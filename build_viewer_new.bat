@echo off
rem Compila il nuovo visualizzatore docxview_new.exe con litedocx.dll (tcc).
rem Prima fa il build incrementale di litedocx, poi copia la DLL e compila il viewer.
setlocal
set TCC="C:\Program Files (x86)\tcc64\tcc.exe"

rem ============================================================
rem 1/2 Build incrementale litedocx.dll (delega a PS che ha gia logica incrementale)
rem ============================================================
echo ============================================================
echo [1/2] Build litedocx.dll (incrementale)...
echo ============================================================
call .\litedocx\build_litedocx.bat
if errorlevel 1 (
    echo [ERRORE] Build litedocx FALLITA - annullo build viewer
    exit /b 1
)

rem ============================================================
rem 2/2 Copia DLL (solo se build litedocx OK) e compila viewer
rem ============================================================
echo.
echo ============================================================
echo [2/2] Copia DLL e build docxview_new.exe...
echo ============================================================
copy /y ".\litedocx\litedocx.dll" . >nul
copy /y ".\litedocx\liblitedocx.a" . >nul

%TCC% -Wall -I. -mconsole -o docxview_new.exe docxview_new.c litedocx.dll -lgdi32 -luser32 -lkernel32 -lcomdlg32 -lole32 -lwindowscodecs -lmsimg32
if errorlevel 1 goto :fail

echo.
echo Build OK: docxview_new.exe
exit /b 0

:fail
echo.
echo Build FALLITA.
exit /b 1
