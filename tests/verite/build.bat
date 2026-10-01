@echo off
rem Host-side test of the Rendition Verite core with MSVC.
rem Usage: build.bat [path\to\v10002d.uc] [output directory]
rem The microcode ships with the X.org rendition driver (xf86-video-rendition/src/v10002d.uc).
setlocal
set HERE=%~dp0
set OUT=%~2
if "%OUT%"=="" set OUT=%CD%
if not defined VCINSTALLDIR (
	for %%v in (2022 2019) do for %%e in (Community Professional Enterprise BuildTools) do (
		if exist "%ProgramFiles%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat" call "%ProgramFiles%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat" >nul & goto :have_vc
		if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat" call "%ProgramFiles(x86)%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat" >nul & goto :have_vc
	)
)
:have_vc
cl /nologo /EHsc /O2 /W4 /wd4100 /Fe"%OUT%\verite_test.exe" /Fo"%OUT%\\" "%HERE%verite_test.cpp" "%HERE%..\..\src\hardware\verite_core.cpp" || exit /b 1
"%OUT%\verite_test.exe" %1 "%OUT%"
