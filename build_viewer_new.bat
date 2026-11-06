@echo off
rem Compila il nuovo visualizzatore docxview_new.exe con litedocx.dll (tcc).
setlocal
set TCC="C:\Program Files (x86)\tcc64\tcc.exe"

REM Usiamo una versione adattata o la nuova logica
REM Per ora compiliamo docxview_new.c se esiste, o copiamo e prepariamo
echo Copia di litedocx.dll e liblitedocx.a...
copy /y ".\\litedocx\\litedocx.dll" . >nul
copy /y ".\\litedocx\\liblitedocx.a" . >nul

"C:\Program Files (x86)\tcc64\tcc.exe" -Wall -I. -o docxview_new.exe docxview_new.c litedocx.dll -lgdi32 -luser32 -lkernel32 -lcomdlg32 -lole32 -lwindowscodecs -lmsimg32
if errorlevel 1 goto :fail

echo.
echo Build OK: docxview_new.exe
exit /b 0

:fail
echo.
echo Build FALLITA.
exit /b 1
