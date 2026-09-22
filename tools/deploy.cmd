@echo off
rem Thin transport shim (Devkit owns the mechanism; ADR-0046 hook pattern):
rem resolve the devkit checkout, then run the canonical deploy script with
rem this repository as the target.
setlocal
if "%QIVEN_DEVKIT_ROOT%"=="" (
  set "QIVEN_DEVKIT_ROOT=%~dp0..\..\qiven-devkit"
)
if not exist "%QIVEN_DEVKIT_ROOT%\tools\deploy_bundle.py" (
  echo [FAIL] deploy: qiven-devkit not found at %QIVEN_DEVKIT_ROOT% - set QIVEN_DEVKIT_ROOT
  exit /b 2
)
python "%QIVEN_DEVKIT_ROOT%\tools\deploy_bundle.py" --repo "%~dp0.." %*
exit /b %ERRORLEVEL%
