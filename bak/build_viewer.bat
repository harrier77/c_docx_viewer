@echo off
rem Compila il visualizzatore docxview.exe (tcc).
rem Richiede litehtml_c.dll accanto all'exe oppure nella cartella ..\sunburst\.
setlocal
set TCC="C:\Program Files (x86)\tcc64\tcc.exe"
set SRC=docx.c zip.c inflate.c xmlbuf.c htmlbuf.c wml.c css.c

%TCC% -Wall -o docxview.exe docxview.c %SRC% -lgdi32 -luser32 -lkernel32 -lcomdlg32
if errorlevel 1 goto :fail

echo.
echo Build OK: docxview.exe
echo Uso: docxview.exe [file.docx]
exit /b 0

:fail
echo.
echo Build FALLITA.
exit /b 1
