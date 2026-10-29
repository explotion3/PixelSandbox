@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "editor_pwsh="
for %%P in (pwsh.exe) do set "editor_pwsh=%%~$PATH:P"
if defined editor_pwsh goto launch
for %%P in ("%ProgramFiles%\PowerShell\7\pwsh.exe" "%LOCALAPPDATA%\Microsoft\PowerShell\7\pwsh.exe" "%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\powershell\pwsh.exe") do (
    if not defined editor_pwsh if exist "%%~fP" set "editor_pwsh=%%~fP"
)
if defined editor_pwsh goto launch
echo PowerShell 7 was not found in PATH or the known installation locations.
echo Install PowerShell 7 or add its pwsh.exe directory to PATH.
set "launch_exit=9009"
goto finished

:launch
pushd "%~dp0"
"%editor_pwsh%" -NoLogo -NoProfile -File "%~dp0tools\project.ps1" editor %*
set "launch_exit=%errorlevel%"
popd

:finished
if not "%launch_exit%"=="0" (
    echo.
    echo Editor launch failed. Check the messages above.
    pause
)
exit /b %launch_exit%
