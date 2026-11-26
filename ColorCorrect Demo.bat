@echo off
cd /d "%~dp0dist\Welder"
start "" "%~dp0dist\Welder\bin\welder_ui.exe" "%~dp0dist\Welder\examples\colorcorrect.weld" --view ColorCorrect1 --props ColorCorrect1
