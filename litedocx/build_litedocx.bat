@echo off
rem ===========================================================================
rem  build_litedocx.bat - builds litedocx.dll with MinGW-w64.
rem  Delegates everything to psbuild.ps1 for reliable path/quote handling.
rem ===========================================================================
setlocal enabledelayedexpansion

set "MINGW=C:\Users\pr30565\Nim_code\mingw64\bin"
set "GCC=%MINGW%\gcc.exe"
set "GXX=%MINGW%\g++.exe"
set "HERE=%~dp0"
rem Remove trailing backslash for PowerShell
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
set "ROOT=%HERE%\.."
set "LITEHTML=%ROOT%\litehtml"
set "OBJ=%HERE%obj"

if not exist "%GCC%" (
  echo [ERRORE] gcc non trovato in "%GCC%"
  exit /b 1
)
if not exist "%LITEHTML%\include\litehtml.h" (
  echo [ERRORE] Progetto litehtml non trovato in "%LITEHTML%"
  exit /b 1
)

if not exist "%HERE%psbuild.ps1" (
  echo [ERRORE] psbuild.ps1 non trovato
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dpn0.ps1" ^
  -Root "%ROOT%" -Litehtml "%LITEHTML%" -Obj "%OBJ%" ^
  -Gcc "%GCC%" -Gxx "%GXX%" -Here "%HERE%"

exit /b %ERRORLEVEL%
