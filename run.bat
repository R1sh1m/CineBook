@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

set "EXE_PATH=%SCRIPT_DIR%\cinebook.exe"
set "MSYS_ROOT="

for %%D in ("C:\msys64" "C:\msys2" "%USERPROFILE%\msys64") do (
    if not defined MSYS_ROOT (
        if exist "%%~fD\mingw64\bin\gcc.exe" set "MSYS_ROOT=%%~fD"
    )
)

if not defined MSYS_ROOT (
    echo [run] MSYS2 not found.
    echo [run] Checked: C:\msys64, C:\msys2, %%USERPROFILE%%\msys64
    echo [run] See README prerequisites.
    exit /b 1
)

set "PATH=%MSYS_ROOT%\mingw64\bin;%PATH%"

if not exist "%SCRIPT_DIR%\data" mkdir "%SCRIPT_DIR%\data"
if not exist "%SCRIPT_DIR%\data\db" mkdir "%SCRIPT_DIR%\data\db"
if not exist "%SCRIPT_DIR%\data\idx" mkdir "%SCRIPT_DIR%\data\idx"
if not exist "%SCRIPT_DIR%\exports" mkdir "%SCRIPT_DIR%\exports"

if not exist "%EXE_PATH%" (
    call :build_app
    if errorlevel 1 exit /b 1
)

if not exist "%SCRIPT_DIR%\data\db\users.db" (
    call :seed_db
    if errorlevel 1 exit /b 1
)

pushd "%SCRIPT_DIR%" >nul
"%EXE_PATH%"
set "APP_EXIT=%ERRORLEVEL%"
popd >nul

if not "%APP_EXIT%"=="0" (
    echo.
    echo [run] cinebook.exe failed to start or exited with error code %APP_EXIT%.
    echo [run] If this is a missing DLL/runtime dependency issue, see README prerequisites.
)

exit /b %APP_EXIT%

:build_app
echo [run] cinebook.exe not found. Building with MSYS2 make...
if not exist "%MSYS_ROOT%\usr\bin\make.exe" (
    echo [run] make.exe not found in %MSYS_ROOT%\usr\bin
    echo [run] See README prerequisites.
    exit /b 1
)

"%MSYS_ROOT%\usr\bin\make.exe" -C "%SCRIPT_DIR%"
if errorlevel 1 (
    echo [run] Build failed.
    echo [run] See README prerequisites.
    exit /b 1
)

if not exist "%EXE_PATH%" (
    echo [run] Build completed but cinebook.exe was not produced.
    echo [run] See README prerequisites.
    exit /b 1
)

exit /b 0

:seed_db
echo [run] users.db not found. Seeding database...

if not exist "%SCRIPT_DIR%\tools\seed.c" (
    echo [run] tools\seed.c not found.
    exit /b 1
)

if not exist "%MSYS_ROOT%\mingw64\bin\gcc.exe" (
    echo [run] gcc.exe not found in %MSYS_ROOT%\mingw64\bin
    echo [run] See README prerequisites.
    exit /b 1
)

"%MSYS_ROOT%\mingw64\bin\gcc.exe" -std=c11 -Wall -o "%SCRIPT_DIR%\seed.exe" "%SCRIPT_DIR%\tools\seed.c"
if errorlevel 1 (
    echo [run] Failed to compile seed.c
    exit /b 1
)

pushd "%SCRIPT_DIR%" >nul
"%SCRIPT_DIR%\seed.exe"
set "SEED_EXIT=%ERRORLEVEL%"
popd >nul

if exist "%SCRIPT_DIR%\seed.exe" del /q "%SCRIPT_DIR%\seed.exe" >nul 2>nul

if not "%SEED_EXIT%"=="0" (
    echo [run] Seeder failed with code %SEED_EXIT%.
    exit /b %SEED_EXIT%
)

exit /b 0