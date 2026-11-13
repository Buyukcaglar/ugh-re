@echo off
rem build.cmd [test|game|all|private] - builds with MSVC x64 (Visual Studio 2022 or later).
rem private: build\private\UGH!64.exe with the game files inside (UGH_GAMEDIR, default
rem _references\dos). That exe contains the game: for your own use only, never publish it.
setlocal
if defined VSCMD_ARG_TGT_ARCH goto have_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%I in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSI=%%I"
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "%VSI%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 2
:have_vs
set "ROOT=%~dp0"
set "B=%ROOT%build"
if not exist "%B%\obj" mkdir "%B%\obj"
set "WHAT=%~1"
if "%WHAT%"=="" set "WHAT=all"
set CFLAGS=/nologo /utf-8 /O2 /W4 /WX /std:c17 /permissive- /D_CRT_SECURE_NO_WARNINGS /wd4996 /wd4310 /Fo"%B%\obj\\" /Fd"%B%\obj\\"
set CORE=%ROOT%src\core\cpu.c %ROOT%src\hal\machine.c %ROOT%src\hal\dos.c %ROOT%src\hal\vga.c %ROOT%src\hal\sb.c %ROOT%src\hal\opl.c %ROOT%src\hal\pack.c %ROOT%src\hal\game.c %ROOT%src\audio\amiga_audio.c %ROOT%src\audio\amiga_data.c %ROOT%src\audio\tfmx.c %ROOT%src\host\pngw.c
set "GEN=%ROOT%src\gen\gen_all.c"
if not exist "%GEN%" set "GEN=%ROOT%src\gen\gen_none.c"
if not exist "%ROOT%src\gen\gen_all.c" echo src\gen\gen_all.c not generated: building the interpreter-only version (see README).
set CORE=%CORE% %GEN% %ROOT%src\gen\gen_rt.c
if /i "%WHAT%"=="test" goto test
if /i "%WHAT%"=="all" goto test
if /i "%WHAT%"=="private" goto private
goto game
:test
cl %CFLAGS% %CORE% %ROOT%src\host\headless\main.c /Fe"%B%\ughtest.exe" /link /nologo || exit /b 1
if /i "%WHAT%"=="test" exit /b 0
:game
if not exist "%ROOT%src\host\win32\host_win32.c" exit /b 0
rc /nologo /fo "%B%\obj\ugh.res" "%ROOT%src\host\win32\ugh.rc" || exit /b 1
cl %CFLAGS% %CORE% %ROOT%src\host\win32\host_win32.c %ROOT%src\host\win32\audio_win32.c %ROOT%src\host\crt.c "%B%\obj\ugh.res" /Fe"%B%\UGH!64.exe" /link /nologo /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%src\host\win32\ugh.manifest" user32.lib gdi32.lib shell32.lib dwmapi.lib uxtheme.lib advapi32.lib ole32.lib comdlg32.lib winmm.lib xinput.lib || exit /b 1
exit /b 0
:private
if "%UGH_GAMEDIR%"=="" set "UGH_GAMEDIR=%ROOT%_references\dos"
if "%UGH_ADF%"=="" for %%F in ("%ROOT%_references\amiga\*.adf") do set "UGH_ADF=%%~fF"
if not exist "%B%\private" mkdir "%B%\private"
if defined UGH_ADF (python "%ROOT%tools\mkpak.py" "%UGH_GAMEDIR%" "%UGH_ADF%" "%B%\obj\game.pak" || exit /b 1) else (python "%ROOT%tools\mkpak.py" "%UGH_GAMEDIR%" "%B%\obj\game.pak" || exit /b 1)
> "%B%\obj\pack.rc" echo 100 RCDATA "game.pak"
rc /nologo /fo "%B%\obj\pack.res" "%B%\obj\pack.rc" || exit /b 1
rc /nologo /fo "%B%\obj\ugh.res" "%ROOT%src\host\win32\ugh.rc" || exit /b 1
cl %CFLAGS% %CORE% %ROOT%src\host\win32\host_win32.c %ROOT%src\host\win32\audio_win32.c %ROOT%src\host\crt.c "%B%\obj\ugh.res" "%B%\obj\pack.res" /Fe"%B%\private\UGH!64.exe" /link /nologo /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%src\host\win32\ugh.manifest" user32.lib gdi32.lib shell32.lib dwmapi.lib uxtheme.lib advapi32.lib ole32.lib comdlg32.lib winmm.lib xinput.lib || exit /b 1
exit /b 0
