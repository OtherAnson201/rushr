@echo off
setlocal

rem Builds Glory.sln for x64 and leaves the exe in build\Glory.exe
rem Usage: build.bat [Release or Debug] [rebuild]

set "ROOT=%~dp0"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

set "TARGET=Build"
if /i "%~2"=="rebuild" set "TARGET=Rebuild"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs

rem Newest Visual Studio / Build Tools install that has the C++ compiler
set "VSFILTER=-latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64"

set "MSBUILD="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" %VSFILTER% -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD goto :no_vs

set "VSMAJOR="
for /f "usebackq tokens=1 delims=." %%i in (`"%VSWHERE%" %VSFILTER% -property installationVersion`) do set "VSMAJOR=%%i"

rem The project asks for the v145 toolset (VS 2026). Fall back to the one that ships with the install found.
set "TOOLSET=v145"
if "%VSMAJOR%"=="17" set "TOOLSET=v143"
if "%VSMAJOR%"=="16" set "TOOLSET=v142"

echo [build] %CONFIG% x64, toolset %TOOLSET%
echo [build] %MSBUILD%

"%MSBUILD%" "%ROOT%Glory.sln" /nologo /m /v:minimal /t:%TARGET% /p:Configuration=%CONFIG% /p:Platform=x64 /p:PlatformToolset=%TOOLSET%
if errorlevel 1 goto :failed

echo.
echo [build] OK: %ROOT%build\Glory.exe
exit /b 0

:failed
echo.
echo [build] FAILED
exit /b 1

:no_vs
echo [build] No Visual Studio or Build Tools install with the C++ workload was found.
echo [build] Install "Desktop development with C++" from the Visual Studio Installer.
exit /b 1
