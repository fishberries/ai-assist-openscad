@echo off
setlocal enabledelayedexpansion

set "SOURCE_ROOT=%~dp0\.."
pushd "%SOURCE_ROOT%"
set "SOURCE_ROOT=%CD%"
set "WORK_ROOT=%SOURCE_ROOT%"
if "%BUILD_ROOT%"=="" set "BUILD_ROOT=%SystemDrive%\openscad-build"
if "%INSTALL_ROOT%"=="" set "INSTALL_ROOT=%SystemDrive%\openscad-vcpkg-installed"

echo(%SOURCE_ROOT% | findstr /c:" " >nul
if not errorlevel 1 (
  set "JUNCTION_ROOT=%SystemDrive%\openscad-msvc-src"
  if exist "!JUNCTION_ROOT!" (
    rmdir "!JUNCTION_ROOT!"
  )
  mklink /J "!JUNCTION_ROOT!" "%SOURCE_ROOT%" >nul
  if errorlevel 1 (
    echo Failed to create no-space junction at !JUNCTION_ROOT!
    exit /b 1
  )
  popd
  pushd "!JUNCTION_ROOT!"
  set "WORK_ROOT=%CD%"
)

if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=C:\vcpkg"
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  echo VCPKG_ROOT is not set to a valid vcpkg install. Current value: %VCPKG_ROOT%
  exit /b 1
)

if "%CMAKE_EXE%"=="" set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
if not exist "%CMAKE_EXE%" set "CMAKE_EXE=cmake"

if "%SHELL_EXE%"=="" set "SHELL_EXE=%ProgramFiles%\Git\bin\bash.exe"
if not exist "%SHELL_EXE%" (
  echo Git Bash was not found. Set SHELL_EXE or install Git for Windows.
  exit /b 1
)

if "%FLEX_EXE%"=="" set "FLEX_EXE=%LOCALAPPDATA%\Microsoft\WinGet\Packages\WinFlexBison.win_flex_bison_Microsoft.Winget.Source_8wekyb3d8bbwe\win_flex.exe"
if not exist "%FLEX_EXE%" (
  echo win_flex.exe was not found. Set FLEX_EXE or install WinFlexBison.
  exit /b 1
)

if "%BISON_EXE%"=="" set "BISON_EXE=%LOCALAPPDATA%\Microsoft\WinGet\Packages\WinFlexBison.win_flex_bison_Microsoft.Winget.Source_8wekyb3d8bbwe\win_bison.exe"
if not exist "%BISON_EXE%" (
  echo win_bison.exe was not found. Set BISON_EXE or install WinFlexBison.
  exit /b 1
)

echo Installing vcpkg dependencies into %INSTALL_ROOT%...
"%VCPKG_ROOT%\vcpkg.exe" install --triplet x64-windows --x-install-root="%INSTALL_ROOT%"
if errorlevel 1 exit /b %errorlevel%

if not exist "%BUILD_ROOT%" mkdir "%BUILD_ROOT%"

rem This fork expects the full GUI build, including the AI panel.
"%CMAKE_EXE%" -B "%BUILD_ROOT%" -S "%WORK_ROOT%" -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
  "-DVCPKG_INSTALLED_DIR=%INSTALL_ROOT:\=/%" ^
  "-DSHELL_EXE=%SHELL_EXE:\=/%" ^
  "-DFLEX_EXECUTABLE=%FLEX_EXE:\=/%" ^
  "-DBISON_EXECUTABLE=%BISON_EXE:\=/%" ^
  -DUSE_QT6=ON ^
  -DHEADLESS=OFF ^
  -DENABLE_TESTS=OFF ^
  -DENABLE_GUI_TESTS=OFF ^
  -DUSE_BUILTIN_OPENCSG=TRUE ^
  -DENABLE_QTDBUS=OFF ^
  -DENABLE_GAMEPAD=OFF ^
  -DCMAKE_EXE_LINKER_FLAGS="/manifest:no" ^
  -DCMAKE_MODULE_LINKER_FLAGS="/manifest:no" ^
  -DCMAKE_SHARED_LINKER_FLAGS="/manifest:no"
if errorlevel 1 exit /b %errorlevel%

if /I "%BUILD_DEBUG%"=="1" (
  "%CMAKE_EXE%" --build "%BUILD_ROOT%" --config Debug --parallel
  if errorlevel 1 exit /b %errorlevel%
)

"%CMAKE_EXE%" --build "%BUILD_ROOT%" --config Release --parallel
set BUILD_EXIT=%errorlevel%
popd
exit /b %BUILD_EXIT%
