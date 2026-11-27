@echo off
rem Launch the workspace's Windows package when present, otherwise use the local build.
if exist "%~dp0build\welder-release\welder-0.2.6-win64\Welder.bat" (
    call "%~dp0build\welder-release\welder-0.2.6-win64\Welder.bat" %*
    exit /b
)
cd /d "%~dp0dist\Welder"
start "" "%~dp0dist\Welder\bin\welder_ui.exe" %*
