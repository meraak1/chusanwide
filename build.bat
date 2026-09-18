@echo off
rem Build chusanwide.dll on Windows with Visual Studio.
rem Needs: Visual Studio with the "Desktop development with C++" workload,
rem        CMake, and Git (MinHook is fetched automatically).
setlocal
cd /d "%~dp0"

cmake -B build -A Win32
if errorlevel 1 goto fail

cmake --build build --config Release
if errorlevel 1 goto fail

echo.
echo Built: build\Release\chusanwide.dll
echo.
pause
exit /b 0

:fail
echo.
echo Build failed. See the messages above.
echo.
pause
exit /b 1
