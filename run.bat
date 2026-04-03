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
    echo   MSYS2 not found. Attempting to download and install MSYS2 automatically...
    set "MSYS2_INSTALLER_URL=https://github.com/msys2/msys2-installer/releases/latest/download/msys2-x86_64-latest.exe"
    set "MSYS2_INSTALLER=msys2-x86_64-latest.exe"
    if exist "%MSYS2_INSTALLER%" del "%MSYS2_INSTALLER%"
    powershell -Command "try { Invoke-WebRequest -Uri '%MSYS2_INSTALLER_URL%' -OutFile '%MSYS2_INSTALLER%' -UseBasicParsing } catch { exit 1 }"
    if not exist "%MSYS2_INSTALLER%" (
        echo   ERROR: Failed to download MSYS2 installer.
        echo   Please download and install MSYS2 manually from https://www.msys2.org/
        pause
        exit /b 1
    )
    echo   Running MSYS2 installer...
    start /wait "" "%CD%\%MSYS2_INSTALLER%"
    echo   Please complete the MSYS2 installation in the installer window.
    echo   After installation, re-run this script.
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
