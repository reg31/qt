@echo off
setlocal enabledelayedexpansion
set GXX=C:\ProgramData\mingw64\mingw64\bin\x86_64-w64-mingw32-g++.exe
set ARGS=
:loop
if "%~1"=="" goto done
set "ARG=%~1"
if "!ARG:~0,21!"=="-Wl,--version-script," goto skip
set "ARGS=!ARGS! "%~1""
:skip
shift
goto loop
:done
%GXX% !ARGS!
exit /b %ERRORLEVEL%
