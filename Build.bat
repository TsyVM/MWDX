@echo off
setlocal

:: ─────────────────────────────────────────────────────────────────────────────
:: MWDX Build Script
:: Builds d3d9.dll (x86 Release) for NFS Most Wanted 2005 v1.3
:: ─────────────────────────────────────────────────────────────────────────────

:: Locate DXSDK
if "%DXSDK_DIR%"=="" (
    for /f "tokens=2*" %%A in (
        'reg query "HKLM\SOFTWARE\Microsoft\DirectX SDK" /v "InstallPath" 2^>nul'
    ) do set "DXSDK_DIR=%%B"
)
if "%DXSDK_DIR%"=="" (
    for /f "tokens=2*" %%A in (
        'reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\DirectX SDK" /v "InstallPath" 2^>nul'
    ) do set "DXSDK_DIR=%%B"
)
:: The installer does not always leave DXSDK_DIR behind - look where it puts
:: itself before giving up.
if "%DXSDK_DIR%"=="" (
    if exist "%ProgramFiles(x86)%\Microsoft DirectX SDK (June 2010)\Lib\x86\d3d9.lib" (
        set "DXSDK_DIR=%ProgramFiles(x86)%\Microsoft DirectX SDK (June 2010)"
    ) else if exist "%ProgramFiles%\Microsoft DirectX SDK (June 2010)\Lib\x86\d3d9.lib" (
        set "DXSDK_DIR=%ProgramFiles%\Microsoft DirectX SDK (June 2010)"
    )
)
if "%DXSDK_DIR%"=="" (
    echo ERROR: DirectX June 2010 SDK not found. Set DXSDK_DIR manually.
    pause & exit /b 1
)
echo Using DXSDK: %DXSDK_DIR%

set BUILD_DIR=build_win32
cmake -S . -B %BUILD_DIR% -G "Visual Studio 17 2022" -A Win32 -DDXSDK_DIR="%DXSDK_DIR%"
if errorlevel 1 ( echo CMake configure failed. & pause & exit /b 1 )

cmake --build %BUILD_DIR% --config Release
if errorlevel 1 ( echo Build failed. & pause & exit /b 1 )

echo.
echo ── Build complete ──
echo Output: %BUILD_DIR%\Release\d3d9.dll
echo Copy d3d9.dll and MWDX.ini to your NFS:MW game folder.
pause
