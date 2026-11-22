@echo off
REM ============================================================================
REM  install.bat - Install background-effect-tools into OBS Studio (Windows x64).
REM
REM  Follows the official OBS plugin-location guidance:
REM    https://obsproject.com/kb/legacy-plugin-developer-info
REM
REM  What it does:
REM    1. Finds OBS (argument > registry "HKLM\SOFTWARE\OBS Studio" >
REM       %ProgramFiles%\obs-studio) and reads the version of obs64.exe.
REM    2. Refuses to run while OBS is running.
REM    3. Removes old copies from the legacy Program Files location:
REM         <obs>\obs-plugins\64bit\background-effect-tools.dll / .pdb
REM         <obs>\data\obs-plugins\background-effect-tools\
REM    4. Installs into %ProgramData%\obs-studio\plugins\background-effect-tools\ with
REM       the DLL in BOTH places OBS looks for it:
REM         background-effect-tools.dll            (OBS 33+ layout)
REM         bin\64bit\background-effect-tools.dll  (OBS 32 layout; OBS 33 skips it
REM                                    once the one above is loaded)
REM       so it does not depend on detecting the OBS version (several OBS
REM       installs, upgrades 32 -> 33, an OBS the registry does not know).
REM         Portable OBS 33+ : <obs>\plugins\background-effect-tools\ (flat)
REM       The plugin folder is mirrored, so files from older versions are
REM       removed.
REM
REM  Usage:
REM    install.bat                 Install (asks for admin rights via UAC)
REM    install.bat "D:\obs-studio" Install, using this OBS folder
REM    install.bat /check [dir]    Only show what would be done (no admin)
REM    /nopause                    Do not wait for a key at the end, and do
REM                                not self-elevate (for use from scripts)
REM
REM  Every run (except /check) writes install-log.txt next to this file.
REM ============================================================================
setlocal EnableExtensions
cd /d "%~dp0"

set "PLUGIN=background-effect-tools"
set "SRC=%~dp0%PLUGIN%"
set "LOGFILE=%~dp0install-log.txt"
set "CHECK_ONLY=0"
set "NOPAUSE=0"
set "ELEVATED=0"
set "OBS_DIR="

:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="/check"   (set "CHECK_ONLY=1" & shift & goto :parse_args)
if /i "%~1"=="/nopause" (set "NOPAUSE=1" & shift & goto :parse_args)
if /i "%~1"=="/elevated" (set "ELEVATED=1" & shift & goto :parse_args)
set "OBS_DIR=%~1"
shift
goto :parse_args
:args_done

echo === %PLUGIN% installer ===

REM --- package sanity -------------------------------------------------------
if not exist "%SRC%\%PLUGIN%.dll" (
    echo [error] "%SRC%\%PLUGIN%.dll" not found.
    echo         Extract the whole ZIP first and run install.bat from inside it.
    goto :fail
)
if not exist "%SRC%\data\locale\ja-JP.ini" (
    echo [error] "%SRC%\data\" is incomplete. Re-extract the ZIP.
    goto :fail
)

REM --- 1. locate OBS --------------------------------------------------------
set "OBS_FROM=argument"
if defined OBS_DIR goto :have_obs_dir
set "OBS_FROM=registry"
for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\OBS Studio" /ve 2^>nul ^| find "REG_SZ"') do set "OBS_DIR=%%B"
if defined OBS_DIR goto :have_obs_dir
set "OBS_FROM=default path"
set "OBS_DIR=%ProgramFiles%\obs-studio"
:have_obs_dir
if "%OBS_DIR:~-1%"=="\" set "OBS_DIR=%OBS_DIR:~0,-1%"

set "OBS_MAJOR=0"
set "OBS_VERSION=unknown"
set "PORTABLE=0"
if exist "%OBS_DIR%\bin\64bit\obs64.exe" goto :obs_found
if "%OBS_FROM%"=="argument" (
    echo [error] OBS not found at "%OBS_DIR%".
    echo         Pass the folder that contains bin\64bit\obs64.exe, e.g.:
    echo           install.bat "D:\Apps\obs-studio"
    goto :fail
)
REM An installed OBS loads plugins from %ProgramData% wherever it lives, so
REM this is not fatal; only the clean-up of its old copies is skipped.
echo   [note] OBS was not found at "%OBS_DIR%" ^(from %OBS_FROM%^).
echo          Installing for the installed OBS anyway. For a portable OBS, run:
echo            install.bat "D:\path\to\obs-studio"
set "OBS_FROM=%OBS_FROM%, not found"
goto :obs_checked
:obs_found

set "OBS_MAJOR="
set "OBS_VERSION="
for /f "tokens=1,2" %%A in ('powershell -NoProfile -Command "$v=(Get-Item -LiteralPath (Join-Path $env:OBS_DIR 'bin\64bit\obs64.exe')).VersionInfo; Write-Output ('{0} {1}' -f $v.FileMajorPart, $v.ProductVersion)"') do (
    set "OBS_MAJOR=%%A"
    set "OBS_VERSION=%%B"
)
set /a OBS_MAJOR=OBS_MAJOR+0 >nul 2>&1
if not defined OBS_VERSION set "OBS_VERSION=unknown"

if exist "%OBS_DIR%\obs_portable_mode"     set "PORTABLE=1"
if exist "%OBS_DIR%\portable_mode.txt"     set "PORTABLE=1"
if exist "%OBS_DIR%\obs_portable_mode.txt" set "PORTABLE=1"
:obs_checked

REM --- 2. decide destination + layout ---------------------------------------
REM  LAYOUT=flat : <dest>\NAME.dll + <dest>\data\                  (portable OBS 33+)
REM  LAYOUT=both : flat + <dest>\bin\64bit\NAME.dll                 (installed OBS 32 / 33+)
if "%PORTABLE%"=="1" (
    if %OBS_MAJOR% LSS 33 if not "%OBS_MAJOR%"=="0" (
        echo [error] Portable OBS %OBS_VERSION% is not supported by this installer.
        echo         OBS 32 portable only loads plugins from its own obs-plugins
        echo         folder. Update OBS to 33 or later, or install manually.
        goto :fail
    )
    set "DEST=%OBS_DIR%\plugins\%PLUGIN%"
    set "LAYOUT=flat"
) else (
    set "DEST=%ProgramData%\obs-studio\plugins\%PLUGIN%"
    set "LAYOUT=both"
)

set "LEGACY_DLL=%OBS_DIR%\obs-plugins\64bit\%PLUGIN%.dll"
set "LEGACY_PDB=%OBS_DIR%\obs-plugins\64bit\%PLUGIN%.pdb"
set "LEGACY_DATA=%OBS_DIR%\data\obs-plugins\%PLUGIN%"

echo   OBS folder   : "%OBS_DIR%"  ^(from %OBS_FROM%^)
echo   OBS version  : %OBS_VERSION%  portable=%PORTABLE%
echo   Install to   : "%DEST%"  ^(layout %LAYOUT%^)
set "HAS_LEGACY=0"
if exist "%LEGACY_DLL%"  (echo   Legacy copy  : "%LEGACY_DLL%"  ^(will be removed^)  & set "HAS_LEGACY=1")
if exist "%LEGACY_PDB%"  (echo   Legacy copy  : "%LEGACY_PDB%"  ^(will be removed^)  & set "HAS_LEGACY=1")
if exist "%LEGACY_DATA%" (echo   Legacy copy  : "%LEGACY_DATA%\"  ^(will be removed^) & set "HAS_LEGACY=1")
if "%HAS_LEGACY%"=="0" echo   Legacy copy  : none

REM --- 3. OBS must be closed -----------------------------------------------
set "OBS_RUNNING=0"
tasklist /FI "IMAGENAME eq obs64.exe" /NH 2>nul | find /I "obs64.exe" >nul && set "OBS_RUNNING=1"
if "%OBS_RUNNING%"=="1" echo   OBS running  : YES

if "%CHECK_ONLY%"=="1" (
    echo.
    echo [check] Nothing was changed.
    goto :ok
)

if "%OBS_RUNNING%"=="1" (
    echo [error] OBS is running. Close OBS completely and run install.bat again.
    goto :fail
)

REM --- 4. admin rights (ProgramData + Program Files are protected) --------
REM  fltmc needs admin and, unlike "net session", does not depend on the
REM  Server (LanmanServer) service.
fltmc >nul 2>&1
if not errorlevel 1 goto :is_admin

REM Not elevated. With /nopause (called from scripts such as build.bat) we
REM must not spawn a separate window, so just fail. Otherwise relaunch this
REM script elevated via UAC; the new window shows the result.
if "%NOPAUSE%"=="1" goto :need_admin
if "%ELEVATED%"=="1" goto :need_admin
echo.
echo Administrator rights are needed. Requesting them now (UAC prompt)...
echo The installation continues in a new window.
set "SELF=%~f0"
set "SELF_ARG="
if "%OBS_FROM%"=="argument" set "SELF_ARG=%OBS_DIR%"
REM cmd /c ""script" args" keeps paths with spaces, ( ) or & intact.
powershell -NoProfile -Command "$q = [char]34; $arg = '/elevated'; if ($env:SELF_ARG) { $arg += ' ' + $q + $env:SELF_ARG + $q }; $cl = '/c ' + $q + $q + $env:SELF + $q + ' ' + $arg + $q; try { Start-Process -FilePath $env:ComSpec -ArgumentList $cl -Verb RunAs -ErrorAction Stop; exit 0 } catch { exit 1 }"
if errorlevel 1 (
    echo [error] Administrator rights were not granted ^(UAC was cancelled^).
    goto :need_admin
)
endlocal
exit /b 0

:need_admin
echo [error] Administrator privileges are required.
echo         Right-click install.bat and choose "Run as administrator".
goto :fail

:is_admin

REM --- 5. remove legacy copies ---------------------------------------------
if exist "%LEGACY_DLL%"  del /f /q "%LEGACY_DLL%"
if exist "%LEGACY_PDB%"  del /f /q "%LEGACY_PDB%"
if exist "%LEGACY_DATA%" rmdir /s /q "%LEGACY_DATA%"
if exist "%LEGACY_DLL%" (
    echo [error] Could not remove "%LEGACY_DLL%" ^(in use?^).
    goto :fail
)
if exist "%LEGACY_DATA%" (
    echo [error] Could not remove "%LEGACY_DATA%".
    goto :fail
)

REM --- 6. install -----------------------------------------------------------
REM Mirror the whole plugin folder (drops files of older versions), then add
REM the OBS 32 layout copy of the DLL.
robocopy "%SRC%" "%DEST%" /MIR /R:1 /W:1 /NJH /NJS /NDL /NP /NFL >nul
if errorlevel 8 goto :copy_fail
if "%LAYOUT%"=="both" (
    robocopy "%SRC%" "%DEST%\bin\64bit" %PLUGIN%.dll %PLUGIN%.pdb /R:1 /W:1 /NJH /NJS /NDL /NP /NFL >nul
    if errorlevel 8 goto :copy_fail
)

REM Verify (robocopy's exit code alone can't be trusted).
if not exist "%DEST%\%PLUGIN%.dll" goto :copy_fail
if "%LAYOUT%"=="both" if not exist "%DEST%\bin\64bit\%PLUGIN%.dll" goto :copy_fail
if not exist "%DEST%\data\locale\ja-JP.ini" goto :copy_fail

echo.
echo ============================================================
echo   INSTALL COMPLETE
echo ============================================================
if "%HAS_LEGACY%"=="1" (
    echo   Removed      : old copies in "%OBS_DIR%"
) else (
    echo   Removed      : nothing ^(no old copies found^)
)
echo   Installed to : "%DEST%"
echo   Log          : "%LOGFILE%"
echo.
echo   Start OBS and add "Background Effects" from Add Source.
echo ============================================================
if "%NOPAUSE%"=="0" (
    powershell -NoProfile -Command "Add-Type -AssemblyName PresentationFramework; $m = [regex]::Unescape('Background Effect Tools \u306e\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u304c\u5b8c\u4e86\u3057\u307e\u3057\u305f\u3002\n\n\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u5148:\n') + $env:DEST + [regex]::Unescape('\n\nOBS \u3092\u8d77\u52d5\u3057\u3066\u3001[\u30bd\u30fc\u30b9\u306e\u8ffd\u52a0] \u2192 [\u80cc\u666f\u30a8\u30d5\u30a7\u30af\u30c8] \u304b\u3089\u4f7f\u3048\u307e\u3059\u3002'); [void][System.Windows.MessageBox]::Show($m, 'Background Effect Tools', 'OK', 'Information')" >nul 2>&1
)
call :write_log "INSTALL COMPLETE"
goto :ok

:copy_fail
echo [error] Copy into "%DEST%" failed.
goto :fail

:ok
if "%NOPAUSE%"=="0" pause
endlocal
exit /b 0

:fail
if not "%CHECK_ONLY%"=="1" call :write_log "INSTALL FAILED"
echo.
echo ============================================================
echo   *** INSTALL FAILED ***  See the messages above.
echo ============================================================
if "%NOPAUSE%"=="0" (
    powershell -NoProfile -Command "Add-Type -AssemblyName PresentationFramework; $m = [regex]::Unescape('Background Effect Tools \u306e\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u306b\u5931\u6557\u3057\u307e\u3057\u305f\u3002\n\n\u30a6\u30a3\u30f3\u30c9\u30a6\u306b\u8868\u793a\u3055\u308c\u305f\u30e1\u30c3\u30bb\u30fc\u30b8\u3092\u78ba\u8a8d\u3057\u3066\u304f\u3060\u3055\u3044\u3002'); [void][System.Windows.MessageBox]::Show($m, 'Background Effect Tools', 'OK', 'Error')" >nul 2>&1
)
if "%NOPAUSE%"=="0" pause
endlocal
exit /b 1

REM ---------------------------------------------------------------------------
REM  :write_log RESULT - install-log.txt next to this file, for bug reports.
REM ---------------------------------------------------------------------------
:write_log
> "%LOGFILE%" (
    echo %PLUGIN% installer - %~1
    echo date         : %DATE% %TIME%
    echo windows      : %OS% %PROCESSOR_ARCHITECTURE%
    echo package      : "%SRC%"
    echo OBS folder   : "%OBS_DIR%"  ^(from %OBS_FROM%^)
    echo OBS version  : %OBS_VERSION%  portable=%PORTABLE%
    echo install to   : "%DEST%"  ^(layout %LAYOUT%^)
    echo.
    echo --- plugin files now present ---
) 2>nul
if not exist "%LOGFILE%" exit /b 0
for %%D in ("%ProgramData%\obs-studio\plugins" "%OBS_DIR%\plugins") do if exist "%%~D\" (
    for /d %%P in ("%%~D\*") do for %%F in ("%%~P\*.dll" "%%~P\bin\64bit\*.dll") do echo %%~fF>> "%LOGFILE%"
)
if exist "%OBS_DIR%\obs-plugins\64bit\" for %%F in ("%OBS_DIR%\obs-plugins\64bit\*.dll") do echo %%~fF>> "%LOGFILE%"
exit /b 0
