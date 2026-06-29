@echo off
REM ============================================================================
REM  install.bat - Deploy background-effect-tools into the local OBS install.
REM
REM  This does NOT build anything. It simply copies the already-staged plugin
REM  files sitting next to this script into your OBS installation:
REM
REM    .\background-effect-tools\  -> %OBS_DIR%\data\obs-plugins\background-effect-tools\
REM    .\bin\*                     -> %OBS_DIR%\obs-plugins\64bit\
REM
REM  Writing under "Program Files" needs an elevated/admin command prompt, and
REM  OBS must be closed.
REM
REM  Usage:
REM    Right-click install.bat -> "Run as administrator" (or run it from an
REM    Administrator command prompt). Edit OBS_DIR below if OBS is elsewhere.
REM ============================================================================
setlocal

REM Always run relative to the directory this script lives in.
cd /d "%~dp0"

set PLUGIN=background-effect-tools
set OBS_DIR=C:\Program Files\obs-studio
set OBS_DATA_DIR=%OBS_DIR%\data\obs-plugins\%PLUGIN%
set OBS_BIN_DIR=%OBS_DIR%\obs-plugins\64bit

if not exist "%OBS_DIR%" (
    echo [error] OBS not found at "%OBS_DIR%".
    echo         Edit OBS_DIR at the top of install.bat if your install differs.
    goto :error
)

REM Writing under "Program Files" needs elevation. robocopy unhelpfully returns
REM exit code 0 on access-denied, so gate on an explicit admin check instead.
net session >nul 2>&1
if errorlevel 1 (
    echo [error] Administrator privileges are required to install into:
    echo           "%OBS_DIR%"
    echo         Right-click install.bat and choose "Run as administrator",
    echo         and close OBS first.
    goto :error
)

echo === Installing %PLUGIN% into OBS (%OBS_DIR%) ===

REM --- data files: mirror the dedicated plugin folder (prunes stale locales) ---
echo   data -^> "%OBS_DATA_DIR%"
robocopy "%PLUGIN%" "%OBS_DATA_DIR%" /MIR /R:1 /W:1 /NJH /NJS /NDL /NP
if errorlevel 8 goto :error

REM --- binaries: copy only our files (64bit\ is shared with other plugins) ---
echo   bin  -^> "%OBS_BIN_DIR%"
robocopy "bin" "%OBS_BIN_DIR%" %PLUGIN%.dll %PLUGIN%.pdb /R:1 /W:1 /NJH /NJS /NDL /NP
if errorlevel 8 goto :error

REM Verify the copies actually landed (robocopy's exit code can't be trusted).
if not exist "%OBS_BIN_DIR%\%PLUGIN%.dll"       goto :error
if not exist "%OBS_DATA_DIR%\locale\ja-JP.ini"  goto :error

echo.
echo Install succeeded. Restart OBS to load %PLUGIN%.
endlocal
exit /b 0

:error
echo.
echo *** INSTALL FAILED ***
echo     Could not copy into "%OBS_DIR%".
echo     Run install.bat as administrator (Program Files is write-protected),
echo     and make sure OBS is closed.
endlocal
exit /b 1
