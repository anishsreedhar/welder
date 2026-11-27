@echo off
setlocal
rem Launch the local Windows package. Drag a .weld or .nk file here to open it.
set "WELDER_HOME=%~dp0dist\Welder"
if not exist "%WELDER_HOME%\bin\welder_ui.exe" (
    echo Welder cannot start because its Windows application files are missing.
    echo Expected: "%WELDER_HOME%\bin\welder_ui.exe"
    echo Extract the Windows release package into "%WELDER_HOME%" and try again.
    pause
    exit /b 1
)
pushd "%WELDER_HOME%"
if errorlevel 1 (
    echo Welder cannot open its application folder: "%WELDER_HOME%"
    pause
    exit /b 1
)
set "OFX_PLUGIN_PATH=%WELDER_HOME%\ofx-test-plugins;%OFX_PLUGIN_PATH%"
start "" "%WELDER_HOME%\bin\welder_ui.exe" %*
if errorlevel 1 (
    echo Windows could not launch Welder.
    popd
    pause
    exit /b 1
)
popd
endlocal
