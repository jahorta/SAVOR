@echo off
setlocal enableextensions enabledelayedexpansion

set "SOASIM_FILE_PARSER=%~1"
if "%SOASIM_FILE_PARSER%"=="" set "SOASIM_FILE_PARSER=..\..\bin\x64\Debug\SoaSimFileParsing.exe"

set "INPUT_DIR=%~2"
if "%INPUT_DIR%"=="" set "INPUT_DIR=..\inputs"

set "OUTPUT_DIR=%~3"
if "%OUTPUT_DIR%"=="" set "OUTPUT_DIR=..\parsed\ab"

if not exist "%SOASIM_FILE_PARSER%" (
  echo [run_ab] ERROR: SoaSimFileParsing.exe not found at "%SOASIM_FILE_PARSER%"
  echo [run_ab] Usage:
  echo [run_ab]   run_ab.bat [SoaSimFileParsing.exe] [input_dir] [output_dir]
  exit /b 1
)

if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

set "COMMAND=%SOASIM_FILE_PARSER% %INPUT_DIR% %OUTPUT_DIR% --ab-sa3d-port-vs-sa3d-bridge"
echo [run_ab] !COMMAND!
call !COMMAND!
set "EXIT_CODE=%ERRORLEVEL%"

if not "%EXIT_CODE%"=="0" (
  echo [run_ab] FAILED with exit code %EXIT_CODE%
  exit /b %EXIT_CODE%
)

echo [run_ab] Completed successfully.
exit /b 0
