@echo off
setlocal enableextensions enabledelayedexpansion

if "%~1"=="" (
  echo Usage:
  echo   run_ab_slice.bat ^<SoaSimFileParsing.exe^> [slice] [input_dir] [output_root] [dotnet_bridge_exe] [dotnet_bridge_cmd]
  echo.
  echo Example:
  echo   run_ab_slice.bat "build\\bin\\Release\\SoaSimFileParsing.exe" 3
  echo   run_ab_slice.bat "build\\bin\\Release\\SoaSimFileParsing.exe" 3 "SoaSimFileParsing\\inputs" "SoaSimFileParsing\\parsed\\ab" "" "dotnet run --project tools/sa3d_ref_runner/SA3DRefRunner.csproj --"
  exit /b 1
)

set "SOASIM_FILE_PARSER=%~1"
set "SLICE=%~2"
if "%SLICE%"=="" set "SLICE=0"

set "INPUT_DIR=%~3"
if "%INPUT_DIR%"=="" set "INPUT_DIR=SoaSimFileParsing\inputs"

set "OUTPUT_ROOT=%~4"
if "%OUTPUT_ROOT%"=="" set "OUTPUT_ROOT=SoaSimFileParsing\parsed\ab_slices"

set "DOTNET_BRIDGE_EXE=%~5"
set "DOTNET_BRIDGE_CMD=%~6"

set "SLICE_OUTPUT_DIR=%OUTPUT_ROOT%\slice_%SLICE%"
if not exist "%SLICE_OUTPUT_DIR%" mkdir "%SLICE_OUTPUT_DIR%"

echo [run_ab_slice] Running slice %SLICE%
echo [run_ab_slice] parser_exe=%SOASIM_FILE_PARSER%
echo [run_ab_slice] input_dir=%INPUT_DIR%
echo [run_ab_slice] output_dir=%SLICE_OUTPUT_DIR%

set "COMMAND=\"%SOASIM_FILE_PARSER%\" \"%INPUT_DIR%\" \"%SLICE_OUTPUT_DIR%\" --ab-sa3d-port-vs-sa3d-bridge --dotnet-bridge-slice %SLICE%"

if not "%DOTNET_BRIDGE_EXE%"=="" (
  set "COMMAND=!COMMAND! --dotnet-bridge-exe \"%DOTNET_BRIDGE_EXE%\""
)

if not "%DOTNET_BRIDGE_CMD%"=="" (
  set "COMMAND=!COMMAND! --dotnet-bridge-cmd \"%DOTNET_BRIDGE_CMD%\""
)

echo [run_ab_slice] !COMMAND!
call !COMMAND!
set "EXIT_CODE=%ERRORLEVEL%"

if not "%EXIT_CODE%"=="0" (
  echo [run_ab_slice] FAILED with exit code %EXIT_CODE%
  exit /b %EXIT_CODE%
)

echo [run_ab_slice] Completed slice %SLICE%
exit /b 0
