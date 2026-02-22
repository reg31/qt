@echo off
setlocal enabledelayedexpansion
set COMPILER=%1
shift
set ARGS=
:loop
if "%~1"=="" goto done
set "ARG=%~1"
if "!ARG:~0,21!"=="-Wl,--version-script," (
    shift
    goto loop
)
set "ARGS=!ARGS! "%~1""
shift
goto loop
:done
%COMPILER% !ARGS!
exit /b %ERRORLEVEL%
