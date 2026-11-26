@echo off
rem Configures, builds and tests the weave with MSVC and the bundled CMake/Ninja. No game data or GPU needed.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "CM=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "NJ=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
"%CM%\cmake.exe" -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_MAKE_PROGRAM="%NJ%" -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 exit /b 1
"%CM%\cmake.exe" --build "%~dp0build"
if errorlevel 1 exit /b 1
"%CM%\ctest.exe" --test-dir "%~dp0build" --output-on-failure
endlocal
