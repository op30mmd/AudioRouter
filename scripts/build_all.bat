@echo off
REM AudioRouter - Unified Windows Build (MSVC + CMake)
REM Builds server, client, tests using CMake.
setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_ROOT=%SCRIPT_DIR%.."
pushd "%PROJECT_ROOT%"

set "BUILD_DIR=build"
set "BIN_DIR=bin"
set "CONFIG=Release"
set "CLEAN=0"
set "TARGET=all"

:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="/?" goto :help
if /I "%~1"=="-h" goto :help
if /I "%~1"=="--help" goto :help
if /I "%~1"=="--clean" set "CLEAN=1" & shift & goto :parse_args
if /I "%~1"=="clean" set "CLEAN=1" & shift & goto :parse_args
if /I "%~1"=="--debug" set "CONFIG=Debug" & shift & goto :parse_args
if /I "%~1"=="--config" set "CONFIG=%~2" & shift & shift & goto :parse_args
if /I "%~1"=="--server-only" set "TARGET=server" & shift & goto :parse_args
if /I "%~1"=="--client-only" set "TARGET=client" & shift & goto :parse_args
if /I "%~1"=="--tests-only" set "TARGET=tests" & shift & goto :parse_args
shift
goto :parse_args
:args_done

if "%CLEAN%"=="1" (
    echo [INFO] Cleaning %BUILD_DIR% and bin artifacts...
    if exist "%BUILD_DIR%" rmdir /S /Q "%BUILD_DIR%"
)

echo ============================================================
echo  AudioRouter - Windows Unified Build
echo  Config: %CONFIG%  Target: %TARGET%  BuildDir: %BUILD_DIR%
echo ============================================================

where cmake >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] cmake not found. Install CMake 3.20+ and add to PATH.
    popd & exit /b 1
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"

echo [INFO] Generating CMake project (Visual Studio 17 2022 x64)...
cmake -B "%BUILD_DIR%" -A x64 -DCMAKE_BUILD_TYPE=%CONFIG% .
if %ERRORLEVEL% NEQ 0 (
    echo [WARN] VS2022 generator failed, trying VS2019...
    cmake -B "%BUILD_DIR%" -G "Visual Studio 16 2019" -A x64 -DCMAKE_BUILD_TYPE=%CONFIG% .
    if %ERRORLEVEL% NEQ 0 (
        echo [WARN] VS2019 failed, trying default generator...
        cmake -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=%CONFIG% .
        if %ERRORLEVEL% NEQ 0 (
            echo [ERROR] CMake generation failed.
            popd & exit /b 1
        )
    )
)

echo [INFO] Building target %TARGET% (%CONFIG%)...
if /I "%TARGET%"=="all" (
    cmake --build "%BUILD_DIR%" --config %CONFIG% --parallel
) else if /I "%TARGET%"=="server" (
    cmake --build "%BUILD_DIR%" --config %CONFIG% --target audiorouter_server --parallel
) else if /I "%TARGET%"=="client" (
    cmake --build "%BUILD_DIR%" --config %CONFIG% --target audiorouter_client --parallel
) else if /I "%TARGET%"=="tests" (
    cmake --build "%BUILD_DIR%" --config %CONFIG% --target audiorouter_tests --parallel
) else (
    cmake --build "%BUILD_DIR%" --config %CONFIG% --parallel
)

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed.
    popd & exit /b %ERRORLEVEL%
)

echo [INFO] Copying artifacts to %BIN_DIR%\...
for %%F in ("%BUILD_DIR%\src\server\%CONFIG%\audiorouter_server.exe" "%BUILD_DIR%\bin\%CONFIG%\audiorouter_server.exe" "%BUILD_DIR%\src\server\audiorouter_server.exe") do (
    if exist %%F copy /Y %%F "%BIN_DIR%\" >nul
)
for %%F in ("%BUILD_DIR%\src\client\%CONFIG%\audiorouter_client.exe" "%BUILD_DIR%\bin\%CONFIG%\audiorouter_client.exe" "%BUILD_DIR%\src\client\audiorouter_client.exe") do (
    if exist %%F copy /Y %%F "%BIN_DIR%\" >nul
)
for %%F in ("%BUILD_DIR%\tests\%CONFIG%\audiorouter_tests.exe" "%BUILD_DIR%\bin\%CONFIG%\audiorouter_tests.exe" "%BUILD_DIR%\tests\audiorouter_tests.exe") do (
    if exist %%F copy /Y %%F "%BIN_DIR%\" >nul
)

echo.
dir "%BIN_DIR%\audiorouter_*.exe" 2>nul || echo [WARN] No binaries found in %BIN_DIR%

if /I "%TARGET%"=="tests" (
    if exist "%BIN_DIR%\audiorouter_tests.exe" (
        echo [INFO] Running tests...
        "%BIN_DIR%\audiorouter_tests.exe"
    )
)
if /I "%TARGET%"=="all" (
    if exist "%BIN_DIR%\audiorouter_tests.exe" (
        echo [INFO] Running tests...
        "%BIN_DIR%\audiorouter_tests.exe"
    )
)

echo.
echo ============================================================
echo  Build complete - artifacts in %BIN_DIR%\
echo ============================================================
echo  Server: %BIN_DIR%\audiorouter_server.exe
echo  Client: %BIN_DIR%\audiorouter_client.exe
echo  Tests : %BIN_DIR%\audiorouter_tests.exe
echo ============================================================

popd
exit /b 0

:help
echo Usage: %~nx0 [options]
echo.
echo Options:
echo   --clean          Clean build directory before build
echo   --debug          Build Debug (default Release)
echo   --config NAME    Release, Debug, RelWithDebInfo
echo   --server-only    Only build server
echo   --client-only    Only build client
echo   --tests-only     Only build and run tests
echo   -h, /?, --help   Show help
echo.
echo Examples:
echo   %~nx0
echo   %~nx0 --clean --server-only
echo   %~nx0 --debug
exit /b 0
