@echo off
rem Verifica di compilazione della libreria docx2html e degli strumenti di
rem collaudo in dev\. La libreria NON produce un eseguibile: i suoi .c vanno
rem aggiunti alla riga tcc dell'applicazione host (sunburst, ecc.).
setlocal
set TCC="C:\Program Files (x86)\tcc64\tcc.exe"
set SRC=docx.c zip.c inflate.c xmlbuf.c htmlbuf.c wml.c css.c

%TCC% -Wall -c %SRC%
if errorlevel 1 goto :fail

if not exist dev mkdir dev
%TCC% -Wall -I. -o dev\dump_html.exe dev\dump_html.c %SRC%
if errorlevel 1 goto :fail
%TCC% -Wall -I. -o dev\check_litehtml.exe dev\check_litehtml.c %SRC%
if errorlevel 1 goto :fail

%TCC% -Wall -I. -o dev\check_justify.exe dev\check_justify.c -lgdi32 -luser32
if errorlevel 1 goto :fail
%TCC% -Wall -I. -o dev\zip_test.exe dev\zip_test.c zip.c inflate.c
if errorlevel 1 goto :fail

del /Q *.o >nul 2>nul

echo.
echo Build OK: libreria compilata, dev\dump_html.exe, dev\check_litehtml.exe e dev\check_justify.exe pronti.
exit /b 0

:fail
echo.
echo Build FALLITA.
exit /b 1
