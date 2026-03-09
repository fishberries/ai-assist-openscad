@echo off
setlocal

set "SOURCE=%~1"
if "%SOURCE%"=="" set "SOURCE=C:\openscad-build-2\Release"

set "TARGET=%~2"
if "%TARGET%"=="" set "TARGET=%~dp0..\portable\OpenSCAD-AI-Windows-x64"

if not exist "%SOURCE%\openscad.exe" (
  echo Missing source build folder: "%SOURCE%"
  exit /b 1
)

if exist "%TARGET%" rmdir /s /q "%TARGET%"
mkdir "%TARGET%"

robocopy "%SOURCE%" "%TARGET%" /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP /XF *.pdb openscadinternal.lib svg.lib
if errorlevel 8 exit /b %errorlevel%

copy /y "%~dp0README-AI-BUILD.txt" "%TARGET%\README-AI-BUILD.txt" >nul

exit /b 0
