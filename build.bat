@echo off
setlocal

rem MSBuild's CL task fills a case-insensitive environment dictionary and fails with MSB6001
rem ("HTTPS_PROXY" vs "https_proxy") when both spellings are present. Compiling needs no proxy,
rem and setlocal keeps this out of the caller's environment.
set "HTTP_PROXY="
set "HTTPS_PROXY="
set "http_proxy="
set "https_proxy="
set "ALL_PROXY="
set "all_proxy="

rem ---------------------------------------------------------------------------
rem  todolist4win build helper
rem
rem  Every build produces BOTH configurations, Debug and Release.
rem    app  : bin\Debug\todolist4win.exe        bin\Release\todolist4win.exe
rem    test : bin\Debug\todolist4win_tests.exe  bin\Release\todolist4win_tests.exe
rem
rem  Double-click  : builds both projects in both configurations, runs the tests, then pauses.
rem  Command line  :
rem    build.bat            same as "build.bat all"
rem    build.bat test       build the test project in both configurations and run it
rem    build.bat app        build the application in both configurations
rem    build.bat all        build both projects in both configurations
rem    build.bat start      run the already built application (Debug by default)
rem    build.bat start Release
rem    build.bat clean      delete bin\ and obj\
rem ---------------------------------------------------------------------------

set "ROOT=%~dp0"
set "SLN=%ROOT%todolist4win.sln"
set "PLAT=x64"
set "PAUSE_AT_END="
if "%~1"=="" set "PAUSE_AT_END=1"

call :find_msbuild
if errorlevel 1 exit /b 1

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=all"

if /i "%TARGET%"=="test"  goto :do_test
if /i "%TARGET%"=="app"   goto :do_app
if /i "%TARGET%"=="all"   goto :do_all
if /i "%TARGET%"=="start" goto :do_start
if /i "%TARGET%"=="clean" goto :do_clean

echo Unknown target: %TARGET%
echo Usage: build.bat [test ^| app ^| all ^| start ^| clean]
exit /b 1

rem ---------------------------------------------------------------------------
:do_test
call :build_both todolist4win_tests
if errorlevel 1 goto :fail
echo.
echo === Running tests ===
"%ROOT%bin\Debug\todolist4win_tests.exe"
if errorlevel 1 goto :fail_tests
"%ROOT%bin\Release\todolist4win_tests.exe"
if errorlevel 1 goto :fail_tests
if defined PAUSE_AT_END pause
exit /b 0

:do_app
call :build_both todolist4win
if errorlevel 1 goto :fail
exit /b 0

:do_all
call :build_both todolist4win_tests
if errorlevel 1 goto :fail
call :build_both todolist4win
if errorlevel 1 goto :fail
echo.
echo === Running tests ===
"%ROOT%bin\Debug\todolist4win_tests.exe"
if errorlevel 1 goto :fail_tests
"%ROOT%bin\Release\todolist4win_tests.exe"
if errorlevel 1 goto :fail_tests
if defined PAUSE_AT_END pause
exit /b 0

:do_start
set "CFG=Debug"
if /i "%~2"=="release" set "CFG=Release"
if not exist "%ROOT%bin\%CFG%\todolist4win.exe" (
  echo Application not built yet. Run: build.bat app
  goto :fail
)
"%ROOT%bin\%CFG%\todolist4win.exe"
exit /b %ERRORLEVEL%

:do_clean
if exist "%ROOT%bin" rmdir /s /q "%ROOT%bin"
if exist "%ROOT%obj" rmdir /s /q "%ROOT%obj"
echo Cleaned bin and obj.
exit /b 0

:fail_tests
echo.
echo [FAILED] tests
if defined PAUSE_AT_END pause
exit /b 1

:fail
if defined PAUSE_AT_END pause
exit /b 1

rem ---------------------------------------------------------------------------
:build_both
call :build %~1 Debug
if errorlevel 1 exit /b 1
call :build %~1 Release
if errorlevel 1 exit /b 1
exit /b 0

:build
echo.
echo === Building %~1 [%~2^|%PLAT%] ===
"%MSBUILD%" "%SLN%" /t:%~1 /p:Configuration=%~2 /p:Platform=%PLAT% /v:minimal /nologo
if errorlevel 1 (
  echo [FAILED] %~1 [%~2]
  exit /b 1
)
echo [OK] %~1 [%~2]
exit /b 0

rem ---------------------------------------------------------------------------
:find_msbuild
set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "delims=" %%i in ('"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find MSBuild\**\Bin\MSBuild.exe') do set "MSBUILD=%%i"
)
if not defined MSBUILD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD (
  echo [ERROR] MSBuild not found. Install Visual Studio with the "Desktop development with C++" workload.
  exit /b 1
)
echo Using MSBuild: %MSBUILD%
exit /b 0
