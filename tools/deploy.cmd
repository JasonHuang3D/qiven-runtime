@echo off
rem Thin transport wrapper (Devkit owns the mechanism): the WR-6 launcher in
rem tools/deploy.py performs the workspace bootstrap identity-check BEFORE
rem any Devkit code runs - no QIVEN_DEVKIT_ROOT variable, no sibling
rem fallback, no consumer-local pin; the lock's qiven-devkit node is the
rem only admitted source.
setlocal
python "%~dp0deploy.py" %*
exit /b %ERRORLEVEL%
