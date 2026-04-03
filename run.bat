@echo off
REM CineBook auto-setup launcher (Windows)

echo.
echo   CineBook : Auto Setup ^& Launch
echo   ----------------------------------

SET MSYS2_PATH=
IF EXIST "C:\msys64\usr\bin\bash.exe"           SET MSYS2_PATH=C:\msys64
IF EXIST "C:\msys2\usr\bin\bash.exe"            SET MSYS2_PATH=C:\msys2
IF EXIST "%USERPROFILE%\msys64\usr\bin\bash.exe" SET MSYS2_PATH=%USERPROFILE%\msys64

IF "%MSYS2_PATH%"=="" (
    echo.
    echo   ERROR: MSYS2 not found.
    echo.
    echo   Please install MSYS2 from https://www.msys2.org/
    echo   Then open "MSYS2 MinGW 64-bit" and run:  bash run.sh
    echo.
    pause
    exit /b 1
)

echo   Found MSYS2 at: %MSYS2_PATH%

REM Delegate to bash run.sh inside MSYS2 MinGW64.
SET MSYS_NO_PATHCONV=1

"%MSYS2_PATH%\msys2_shell.cmd" -mingw64 -defterm -no-start -c "cd '%CD:\=/%' && bash run.sh"

IF ERRORLEVEL 1 (
    echo.
    echo   Something went wrong. Try running "bash run.sh" directly
    echo   inside the MSYS2 MinGW 64-bit terminal.
    pause
)
