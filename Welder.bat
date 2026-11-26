@echo off
rem Launch the locally built Welder editor. Drag a .weld or .nk file here to open it.
cd /d "%~dp0dist\Welder"
start "" "%~dp0dist\Welder\bin\welder_ui.exe" %*
