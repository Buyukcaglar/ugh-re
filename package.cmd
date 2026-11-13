@echo off
rem package.cmd - builds UGH64.exe and packs dist\UGH64-1.0-win64.zip (no game files).
setlocal
set "ROOT=%~dp0"
call "%ROOT%build.cmd" game || exit /b 1
set "D=%ROOT%dist\UGH64-1.0-win64"
if exist "%D%" rmdir /s /q "%D%"
mkdir "%D%"
copy /y "%ROOT%build\UGH64.exe" "%D%\" >nul || exit /b 1
copy /y "%ROOT%README.md" "%D%\" >nul
copy /y "%ROOT%LICENSE" "%D%\LICENSE.txt" >nul
if exist "%ROOT%dist\UGH64-1.0-win64.zip" del "%ROOT%dist\UGH64-1.0-win64.zip"
tar -a -c -f "%ROOT%dist\UGH64-1.0-win64.zip" -C "%ROOT%dist" UGH64-1.0-win64 || exit /b 1
echo %ROOT%dist\UGH64-1.0-win64.zip
exit /b 0
