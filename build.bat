@echo off
REM ============================================================================
REM  build.bat - Configure, build, and stage background-effect-tools into package\
REM
REM  Output layout (package\):
REM    package\background-effect-tools\background-effect-tools.dll   plugin binary
REM    package\background-effect-tools\background-effect-tools.pdb   debug symbols
REM    package\background-effect-tools\data\effects\*.effect
REM    package\background-effect-tools\data\locale\*.ini
REM    package\install.bat / README.md / LICENSE / SOURCE-NOTICE.txt
REM
REM  After staging, the plugin is deployed into the local OBS by running
REM  package\install.bat (the same installer users get): legacy copies in
REM  Program Files are removed and the plugin goes to
REM  %ProgramData%\obs-studio\plugins\background-effect-tools\ .
REM  (Needs an elevated/admin command prompt, and OBS must be closed.)
REM
REM  Usage:
REM    build.bat            Build (incremental), refresh package\, deploy to OBS
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
echo === [1/5] Configure (preset %PRESET%) ===
cmake --preset %PRESET%
if errorlevel 1 goto :error

echo.
echo === [2/5] Build (%CONFIG%) ===
cmake --build --preset %PRESET%
if errorlevel 1 goto :error

echo.
echo === [3/5] Clean stale package\ ===
REM Remove the staged tree so deleted effects/locales never linger.
if exist "%PKG_DIR%" rmdir /s /q "%PKG_DIR%"

echo.
echo === [4/5] Stage into %PKG_DIR%\ ===
cmake --install "%BUILD_DIR%" --config %CONFIG% --component dist --prefix "%PKG_DIR%"
if errorlevel 1 goto :error

echo.
echo === [5/5] Deploy into OBS (package\install.bat) ===
call "%PKG_DIR%\install.bat" /nopause
if errorlevel 1 goto :deploy_error

echo.
echo === Done. Staged tree: ===
dir /s /b "%PKG_DIR%"
echo.
echo Build + package + deploy succeeded.
endlocal
exit /b 0

:deploy_error
echo.
echo *** DEPLOY FAILED ***
echo     See the installer messages above. Run build.bat from an
echo     Administrator command prompt, and make sure OBS is closed.
endlocal
exit /b 1

:error
echo.
echo *** BUILD FAILED (exit code %errorlevel%) ***
endlocal
exit /b 1
