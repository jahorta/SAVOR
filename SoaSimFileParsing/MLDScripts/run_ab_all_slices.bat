@echo off
setlocal enableextensions enabledelayedexpansion

if "%~1"=="" (
  echo Usage:
  echo   run_ab_all_slices.bat ^<SoaSimFileParsing.exe^> [input_dir] [output_root] [dotnet_bridge_exe] [dotnet_bridge_cmd] [start_slice] [end_slice]
  echo.
  echo Defaults:
  echo   start_slice=0
  echo   end_slice=9
  exit /b 1
)

set "SOASIM_FILE_PARSER=%~1"
set "INPUT_DIR=%~2"
if "%INPUT_DIR%"=="" set "INPUT_DIR=SoaSimFileParsing\inputs"

set "OUTPUT_ROOT=%~3"
if "%OUTPUT_ROOT%"=="" set "OUTPUT_ROOT=SoaSimFileParsing\parsed\ab_slices"

set "DOTNET_BRIDGE_EXE=%~4"
set "DOTNET_BRIDGE_CMD=%~5"

set "START_SLICE=%~6"
if "%START_SLICE%"=="" set "START_SLICE=0"

set "END_SLICE=%~7"
if "%END_SLICE%"=="" set "END_SLICE=9"

echo [run_ab_all_slices] parser_exe=%SOASIM_FILE_PARSER%
echo [run_ab_all_slices] input_dir=%INPUT_DIR%
echo [run_ab_all_slices] output_root=%OUTPUT_ROOT%
echo [run_ab_all_slices] slice_range=%START_SLICE%..%END_SLICE%

for /L %%S in (%START_SLICE%,1,%END_SLICE%) do (
  echo.
  echo [run_ab_all_slices] ==== Slice %%S ====
  call "%~dp0run_ab_slice.bat" "%SOASIM_FILE_PARSER%" %%S "%INPUT_DIR%" "%OUTPUT_ROOT%" "%DOTNET_BRIDGE_EXE%" "%DOTNET_BRIDGE_CMD%"
  if errorlevel 1 (
    echo [run_ab_all_slices] FAILED at slice %%S
    exit /b !errorlevel!
  )
)

echo.
echo [run_ab_all_slices] All slices completed successfully.
exit /b 0
