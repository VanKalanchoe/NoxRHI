@echo off
setlocal EnableExtensions

for %%I in ("%~dp0..") do set "ROOT_DIR=%%~fI"
set "BUILD_DIR=%ROOT_DIR%\build"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo No configured build found in "%BUILD_DIR%". Running setup_build.bat...
    set "PREV_NO_PAUSE=%NRI_NO_PAUSE%"
    set "NRI_NO_PAUSE=1"
    call "%~dp0setup_build.bat"
    set "SETUP_ERR=%errorlevel%"
    set "NRI_NO_PAUSE=%PREV_NO_PAUSE%"
    if not "%SETUP_ERR%"=="0" (
        echo Setup failed.
        if /i not "%NRI_NO_PAUSE%"=="1" pause
        exit /b 1
    )
)

echo Building NoxRHI ^(%CONFIG%^)...
cmake --build "%BUILD_DIR%" --config "%CONFIG%" --parallel
if errorlevel 1 (
    echo.
    echo Build failed.
    if /i not "%NRI_NO_PAUSE%"=="1" pause
    exit /b 1
)

echo.
echo Build succeeded: "%BUILD_DIR%\%CONFIG%"
if /i not "%NRI_NO_PAUSE%"=="1" pause
exit /b 0
