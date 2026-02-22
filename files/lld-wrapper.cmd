@echo off
setlocal enabledelayedexpansion
set LINKER=%1
shift
set ARGS=
:loop
if "%~1"=="" goto done
if "%~1"=="--version-script" (
    shift
    shift
    goto loop
)
set "ARG=%~1"
if "!ARG:~0,17!"=="--version-script=" (
    shift
    goto loop
)
set "ARGS=!ARGS! %1"
shift
goto loop
:done
%LINKER% !ARGS!
exit /b %ERRORLEVEL%
