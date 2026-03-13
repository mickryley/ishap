@echo off
setlocal EnableDelayedExpansion

:: ---------------------------------------------------------------------------
:: ishap test runner
:: Usage: run_tests.bat [--release] [--no-stats] [--clean]
::
::   --release   Build in Release mode (default: Debug)
::   --no-stats  Pass ISHAP_DISABLE_STATS=ON to CMake
::   --clean     Delete the build directory before configuring
:: ---------------------------------------------------------------------------

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "BUILD_DIR=%ROOT%\out\build\test-runner"
set "BUILD_TYPE=Debug"
set "DISABLE_STATS=OFF"
set "CLEAN_BUILD=0"

:: Parse arguments
:parse_args
if "%~1"=="" goto done_args
if /i "%~1"=="--release" ( set "BUILD_TYPE=Release" & shift & goto parse_args )
if /i "%~1"=="--no-stats" ( set "DISABLE_STATS=ON"  & shift & goto parse_args )
if /i "%~1"=="--clean"    ( set "CLEAN_BUILD=1"     & shift & goto parse_args )
echo [WARNING] Unknown argument: %~1
shift
goto parse_args
:done_args

:: ---------------------------------------------------------------------------
:: Locate Visual Studio via vswhere
:: ---------------------------------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found. Please install Visual Studio.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (
    `"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`
) do set "VS_PATH=%%i"

if not defined VS_PATH (
    echo [ERROR] No Visual Studio installation with C++ tools found.
    exit /b 1
)

:: Initialise the MSVC x64 environment
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Failed to initialise MSVC x64 environment.
    exit /b 1
)

:: ---------------------------------------------------------------------------
:: Clean
:: ---------------------------------------------------------------------------
if "%CLEAN_BUILD%"=="1" (
    echo [ishap] Cleaning %BUILD_DIR% ...
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
)

:: ---------------------------------------------------------------------------
:: Print config
:: ---------------------------------------------------------------------------
echo.
echo  ishap test runner
echo  -----------------
echo  Build type : %BUILD_TYPE%
echo  Stats      : %DISABLE_STATS% ^(ISHAP_DISABLE_STATS^)
echo  Build dir  : %BUILD_DIR%
echo.

:: ---------------------------------------------------------------------------
:: CMake configure
:: ---------------------------------------------------------------------------
echo [ishap] Configuring...
cmake -S "%ROOT%" -B "%BUILD_DIR%" ^
    -G "Ninja" ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DISHAP_BUILD_TESTS=ON ^
    -DISHAP_DISABLE_STATS=%DISABLE_STATS%
if errorlevel 1 (
    echo.
    echo [ishap] Configure FAILED.
    exit /b 1
)

:: ---------------------------------------------------------------------------
:: Build
:: ---------------------------------------------------------------------------
echo.
echo [ishap] Building...
cmake --build "%BUILD_DIR%" --config %BUILD_TYPE%
if errorlevel 1 (
    echo.
    echo [ishap] Build FAILED.
    exit /b 1
)

:: ---------------------------------------------------------------------------
:: Test
:: ---------------------------------------------------------------------------
echo.
echo [ishap] Running tests...
ctest --test-dir "%BUILD_DIR%" -C %BUILD_TYPE% --output-on-failure
if errorlevel 1 (
    echo.
    echo [ishap] Tests FAILED.
    exit /b 1
)

echo.
echo [ishap] All tests passed.
endlocal
exit /b 0
