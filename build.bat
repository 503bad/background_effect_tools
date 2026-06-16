@echo off
REM ============================================================================
REM  build.bat - Configure, build, and stage background-effect-tools into package\
REM
REM  Output layout (package\):
REM    package\bin\background-effect-tools.dll        plugin binary
REM    package\bin\background-effect-tools.pdb        debug symbols
REM    package\background-effect-tools\effects\*.effect
REM    package\background-effect-tools\locale\*.ini
REM    package\README.md / LICENSE / SOURCE-NOTICE.txt  (release docs)
REM
REM  Usage:
REM    build.bat            Build (incremental) and refresh package\
REM    build.bat clean      Wipe build_x64\ and package\ first, then full build
REM ============================================================================
setlocal

REM Always run from the directory this script lives in.
cd /d "%~dp0"

set PRESET=local
set BUILD_DIR=build_x64
set CONFIG=RelWithDebInfo
set PKG_DIR=package

if /i "%~1"=="clean" (
    echo [clean] Removing %BUILD_DIR%\ and %PKG_DIR%\ ...
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    if exist "%PKG_DIR%"   rmdir /s /q "%PKG_DIR%"
)

echo.
echo === [1/4] Configure (preset %PRESET%) ===
cmake --preset %PRESET%
if errorlevel 1 goto :error

echo.
echo === [2/4] Build (%CONFIG%) ===
cmake --build --preset %PRESET%
if errorlevel 1 goto :error

echo.
echo === [3/4] Clean stale package\ ===
REM Remove the staged tree so deleted effects/locales never linger.
if exist "%PKG_DIR%" rmdir /s /q "%PKG_DIR%"

echo.
echo === [4/4] Stage into %PKG_DIR%\ ===
cmake --install "%BUILD_DIR%" --config %CONFIG% --component dist --prefix "%PKG_DIR%"
if errorlevel 1 goto :error

echo.
echo === Done. Staged tree: ===
dir /s /b "%PKG_DIR%"
echo.
echo Build + package succeeded.
endlocal
exit /b 0

:error
echo.
echo *** BUILD FAILED (exit code %errorlevel%) ***
endlocal
exit /b 1
