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
rem  todolist4win_mysql  build helper
rem
rem  Double-click  : compiles the offline test project, runs it, then pauses.
rem  Command line  :
rem    build.bat test           compile the test project and run it
rem    build.bat app            compile the application (needs MySQL Connector/C++ 8.0 x64)
rem    build.bat all            compile both projects
rem    build.bat start          run the already built application
rem    build.bat clean          delete bin\ and obj\
rem
rem  Append "Release" as a second argument to build Release instead of Debug:
rem    build.bat app Release    -> bin\Release\todolist4win_mysql.exe
rem    build.bat start Release
rem ---------------------------------------------------------------------------

set "ROOT=%~dp0"
set "SLN=%ROOT%todolist4win_mysql.sln"
set "CFG=Debug"
set "PLAT=x64"
set "PAUSE_AT_END="
if "%~1"=="" set "PAUSE_AT_END=1"

call :find_msbuild
if errorlevel 1 exit /b 1

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=test"

rem Optional second argument selects the configuration. Debug stays the default because the test
rem project is the double-click target and only the Debug binaries are meant for local iteration.
if /i "%~2"=="release" set "CFG=Release"
if /i "%~2"=="debug"   set "CFG=Debug"

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
call :build todolist4win_mysql_tests
if errorlevel 1 goto :fail
echo.
echo === Running tests ===
"%ROOT%bin\%CFG%\todolist4win_mysql_tests.exe"
set "RC=%ERRORLEVEL%"
if defined PAUSE_AT_END pause
exit /b %RC%

:do_app
call :find_connector
call :announce_connector
call :build todolist4win_mysql
if errorlevel 1 (
  echo.
  echo If the error mentions mysql/jdbc.h or mysqlcppconn-static.lib, the MySQL
  echo Connector/C++ 8.0 x64 package is missing. Install it to
  echo   C:\Program Files\MySQL\MySQL Connector C++ 8.0
  echo or set MYSQLCONNECTOR_ROOT to the extraction folder before running this script.
  goto :fail
)
exit /b 0

:do_all
call :find_connector
call :announce_connector
call :build todolist4win_mysql_tests
if errorlevel 1 goto :fail
call :build todolist4win_mysql
if errorlevel 1 goto :fail
exit /b 0

:do_start
if not exist "%ROOT%bin\%CFG%\todolist4win_mysql.exe" (
  echo Application not built yet. Run: build.bat app
  goto :fail
)
"%ROOT%bin\%CFG%\todolist4win_mysql.exe"
exit /b %ERRORLEVEL%

:do_clean
if exist "%ROOT%bin" rmdir /s /q "%ROOT%bin"
if exist "%ROOT%obj" rmdir /s /q "%ROOT%obj"
echo Cleaned bin and obj.
exit /b 0

:fail
if defined PAUSE_AT_END pause
exit /b 1

rem ---------------------------------------------------------------------------
:build
echo.
echo === Building %~1 [%CFG%^|%PLAT%] ===
"%MSBUILD%" "%SLN%" /t:%~1 /p:Configuration=%CFG% /p:Platform=%PLAT% /v:minimal /nologo
if errorlevel 1 (
  echo [FAILED] %~1
  exit /b 1
)
echo [OK] %~1
exit /b 0

rem ---------------------------------------------------------------------------
:announce_connector
if defined MYSQLROOT (
  echo Using MySQL Connector/C++: %MYSQLROOT%
  rem MSBuild reads this environment variable through MySqlDependencies.props.
  set "MYSQLCONNECTOR_ROOT=%MYSQLROOT%"
) else (
  echo [WARN] MySQL Connector/C++ 8.0 x64 not found; the application target will fail.
)
exit /b 0

rem ---------------------------------------------------------------------------
:find_connector
set "MYSQLROOT="
if defined MYSQLCONNECTOR_ROOT if exist "%MYSQLCONNECTOR_ROOT%\include\jdbc\mysql_driver.h" set "MYSQLROOT=%MYSQLCONNECTOR_ROOT%"
if not defined MYSQLROOT if exist "C:\Program Files\MySQL\MySQL Connector C++ 8.0\include\jdbc\mysql_driver.h" set "MYSQLROOT=C:\Program Files\MySQL\MySQL Connector C++ 8.0"
if not defined MYSQLROOT if exist "%USERPROFILE%\MySQL\MySQL Connector C++ 8.0\include\jdbc\mysql_driver.h" set "MYSQLROOT=%USERPROFILE%\MySQL\MySQL Connector C++ 8.0"
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
