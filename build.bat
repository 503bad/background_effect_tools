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
REM  After staging, the plugin is also deployed into the local OBS install:
REM    package\background-effect-tools\  -> %OBS_DIR%\data\obs-plugins\background-effect-tools\
REM    package\bin\*                     -> %OBS_DIR%\obs-plugins\64bit\
REM  (Deploying into "Program Files" needs an elevated/admin command prompt.)
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

REM OBS install root + the two deploy targets under it.
set OBS_DIR=C:\Program Files\obs-studio
set OBS_DATA_DIR=%OBS_DIR%\data\obs-plugins\background-effect-tools
set OBS_BIN_DIR=%OBS_DIR%\obs-plugins\64bit

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
echo === [5/5] Deploy into OBS (%OBS_DIR%) ===
if not exist "%OBS_DIR%" (
    echo [warn] OBS not found at "%OBS_DIR%" - skipping deploy.
    echo        Edit OBS_DIR at the top of build.bat if your install differs.
    goto :done
)

REM Writing under "Program Files" needs elevation. robocopy unhelpfully returns
REM exit code 0 on access-denied, so gate on an explicit admin check instead.
net session >nul 2>&1
if errorlevel 1 (
    echo [error] Administrator privileges are required to deploy into:
    echo           "%OBS_DIR%"
    echo         Re-run build.bat from a command prompt opened with
    echo         "Run as administrator", and close OBS first.
    goto :deploy_error
)

REM --- data files: mirror the dedicated plugin folder (prunes stale locales) ---
echo   data -^> "%OBS_DATA_DIR%"
robocopy "%PKG_DIR%\background-effect-tools" "%OBS_DATA_DIR%" /MIR /R:1 /W:1 /NJH /NJS /NDL /NP
if errorlevel 8 goto :deploy_error

REM --- binaries: copy only our files (64bit\ is shared with other plugins) ---
echo   bin  -^> "%OBS_BIN_DIR%"
robocopy "%PKG_DIR%\bin" "%OBS_BIN_DIR%" background-effect-tools.dll background-effect-tools.pdb /R:1 /W:1 /NJH /NJS /NDL /NP
if errorlevel 8 goto :deploy_error

REM Verify the copies actually landed (robocopy's exit code can't be trusted).
if not exist "%OBS_BIN_DIR%\background-effect-tools.dll"  goto :deploy_error
if not exist "%OBS_DATA_DIR%\locale\ja-JP.ini"            goto :deploy_error

:done
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
echo     Could not copy into "%OBS_DIR%".
echo     Run this script from an Administrator command prompt (Program Files
echo     is write-protected), and make sure OBS is closed.
endlocal
exit /b 1

:error
echo.
echo *** BUILD FAILED (exit code %errorlevel%) ***
endlocal
exit /b 1
