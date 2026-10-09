@echo off
rem Builds the SU2000 VR link libraries (mbedTLS, libdatachannel with libjuice and usrsctp) as static x64 Release
rem libraries into obj\webrtc. Run once after "git submodule update --init --recursive vs/mbedtls vs/libdatachannel";
rem the dosbox-x project runs it automatically when obj\webrtc\lib is missing.
setlocal
set "ROOT=%~dp0.."
set "OUT=%ROOT%\obj\webrtc"
if exist "%OUT%\lib\datachannel-static.lib" exit /b 0

set "CMAKE=cmake"
where cmake >nul 2>nul
if errorlevel 1 (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "CMAKE=%%i\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)
set "GEN=-G "Visual Studio 16 2019" -A x64 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_POLICY_DEFAULT_CMP0091=NEW"

if not exist "%ROOT%\vs\libdatachannel\deps\libjuice\CMakeLists.txt" (
  git -C "%ROOT%" submodule update --init vs/mbedtls vs/libdatachannel || exit /b 1
  git -C "%ROOT%\vs\libdatachannel" submodule update --init deps/libjuice deps/usrsctp deps/plog || exit /b 1
  git -C "%ROOT%\vs\mbedtls" submodule update --init || exit /b 1
)

rem libdatachannel needs DTLS-SRTP, which the default mbedTLS configuration leaves out
if not exist "%OUT%" mkdir "%OUT%"
> "%OUT%\mbedtls_user_config.h" echo #define MBEDTLS_SSL_DTLS_SRTP
"%CMAKE%" -S "%ROOT%\vs\mbedtls" -B "%OUT%\build\mbedtls" %GEN% -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF ^
  -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DMBEDTLS_FATAL_WARNINGS=OFF ^
  -DMBEDTLS_USER_CONFIG_FILE="%OUT:\=/%/mbedtls_user_config.h" -DCMAKE_INSTALL_PREFIX="%OUT%\install" || exit /b 1
"%CMAKE%" --build "%OUT%\build\mbedtls" --config Release --target install -- /m /v:m || exit /b 1
rem the same option for everything that includes the installed headers
powershell -NoProfile -Command "$p='%OUT%\install\include\mbedtls\mbedtls_config.h'; (Get-Content $p) -replace '^//#define MBEDTLS_SSL_DTLS_SRTP','#define MBEDTLS_SSL_DTLS_SRTP' | Set-Content $p" || exit /b 1

"%CMAKE%" -S "%ROOT%\vs\libdatachannel" -B "%OUT%\build\libdatachannel" %GEN% -DUSE_MBEDTLS=ON -DNO_MEDIA=ON ^
  -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON -DCMAKE_PREFIX_PATH="%OUT%\install" || exit /b 1
"%CMAKE%" --build "%OUT%\build\libdatachannel" --config Release --target datachannel-static -- /m /v:m || exit /b 1

if not exist "%OUT%\lib" mkdir "%OUT%\lib"
for /r "%OUT%\build\libdatachannel" %%f in (*.lib) do copy /y "%%f" "%OUT%\lib\" >nul
copy /y "%OUT%\install\lib\*.lib" "%OUT%\lib\" >nul
if not exist "%OUT%\lib\datachannel-static.lib" exit /b 1
echo SU2000 VR link libraries built in %OUT%\lib
exit /b 0
