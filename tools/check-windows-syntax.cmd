@echo off
setlocal
if "%~1"=="" (
    echo Usage: check-windows-syntax.cmd QtRoot [vcvars64.bat]
    exit /b 2
)
set "ASTER_VCVARS=%~2"
if not defined ASTER_VCVARS set "ASTER_VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
call "%ASTER_VCVARS%" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0.."
cl /nologo /Zs /EHsc /std:c++17 /Zc:__cplusplus /permissive- /utf-8 /MD /W4 /DQT_CORE_LIB /DQT_GUI_LIB /DQT_NETWORK_LIB /DASTER_TEST_QT_NETWORK /I. /I"%~1\include" /I"%~1\include\QtCore" /I"%~1\include\QtGui" /I"%~1\include\QtNetwork" /I"%~1\mkspecs\win32-msvc" aster\cache\core\*.cpp aster\cache\cache\*.cpp aster\cache\source\*.cpp aster\cache\pipeline\*.cpp tests\*.cpp benchmarks\*.cpp
set "ASTER_RESULT=%ERRORLEVEL%"
popd
exit /b %ASTER_RESULT%
