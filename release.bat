@echo off
rem CoD2BRO.zip: the executable, the compiler folder the first start builds the game code with, an empty assets
rem folder, the README and the licenses. No game code or game data goes in: the player's own game is translated and
rem built on their PC.
setlocal
set "GAME_EXE=CoD2BRO"
cd /d "%~dp0"
set "OUT=out\build\release"
set "STAGE=%OUT%\release"
set "ZIP=%GAME_EXE%.zip"

rem the Visual C++ runtime beside the executable, so a fresh Windows install needs nothing more
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
for /d %%v in ("%VSDIR%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "VCRUNTIME=%%c"
if not exist "%VCRUNTIME%\vcruntime140.dll" (echo Visual C++ runtime not found: install Visual Studio with the C++ workload & goto :fail)

rem Microsoft's shader compiler beside the executable (from the Windows SDK's redistributables). Windows has its own
rem copy; Proton and Wine (the Steam Deck) have a stand-in that cannot build this renderer's shaders, which leaves the
rem picture black after the opening movies.
set "D3DCOMPILER=%ProgramFiles(x86)%\Windows Kits\10\Redist\D3D\x64\d3dcompiler_47.dll"
if not exist "%D3DCOMPILER%" echo WARNING: %D3DCOMPILER% not found: the zip will not run on Proton or Wine. Install the Windows SDK.

rem 7-Zip if it is installed (on PATH or in its usual folder), otherwise the tar.exe that comes with Windows 10 and 11
set "SEVENZIP="
where 7z >nul 2>nul && set "SEVENZIP=7z"
if not defined SEVENZIP if exist "%ProgramFiles%\7-Zip\7z.exe" set "SEVENZIP=%ProgramFiles%\7-Zip\7z.exe"
if not defined SEVENZIP where tar >nul 2>nul || (echo No zip tool found: install 7-Zip from https://www.7-zip.org & goto :fail)

rem build first, so the zip never holds an old executable (build.bat with CI set asks no questions)
echo Building ...
set "CI=1"
call build.bat || goto :fail
set "CI="
if not exist "%OUT%\%GAME_EXE%.exe" (echo %OUT%\%GAME_EXE%.exe is missing & goto :fail)
if not exist "%OUT%\sdk_dir.txt" (echo %OUT%\sdk_dir.txt was not written, update the SDK. & goto :fail)
set /p SDK=<%OUT%\sdk_dir.txt

echo Packing ...
rd /s /q "%STAGE%" 2>nul
mkdir "%STAGE%\assets" || goto :fail
copy /y assets\README.txt "%STAGE%\assets\" >nul || goto :fail
copy /y "%OUT%\%GAME_EXE%.exe" "%STAGE%\" >nul || goto :fail
for %%f in (msvcp140 vcruntime140 vcruntime140_1) do copy /y "%VCRUNTIME%\%%f.dll" "%STAGE%\" >nul || goto :fail
if exist "%D3DCOMPILER%" copy /y "%D3DCOMPILER%" "%STAGE%\" >nul
copy /y README.md "%STAGE%\" >nul || goto :fail
copy /y LICENSE "%STAGE%\LICENSE.txt" >nul || goto :fail
copy /y THIRD_PARTY_NOTICES.md "%STAGE%\" >nul || goto :fail
copy /y "%SDK%\THIRD_PARTY.txt" "%STAGE%\" >nul || goto :fail
if exist "%SDK%\LICENSE" copy /y "%SDK%\LICENSE" "%STAGE%\LICENSE-XboxXBE-Recomp.txt" >nul
mkdir "%STAGE%\font-licenses" || goto :fail
copy /y fonts\licenses\*.txt "%STAGE%\font-licenses\" >nul || goto :fail
rem the compiler folder without its work folder (the build's game code); xbe_recomp.exe needs its own runtime copy,
rem Windows only looks beside the exe it starts
robocopy "%OUT%\compiler" "%STAGE%\compiler" /e /xd "%CD%\%OUT%\compiler\work" /xf *.dll /njh /njs /nfl /ndl >nul
if errorlevel 8 (echo copying the compiler folder failed & goto :fail)
for %%f in (msvcp140 vcruntime140) do copy /y "%VCRUNTIME%\%%f.dll" "%STAGE%\compiler\" >nul || goto :fail

del /q "%ZIP%" 2>nul
pushd "%STAGE%"
if defined SEVENZIP ("%SEVENZIP%" a -tzip "%~dp0%ZIP%" * >nul) else (tar -a -c -f "%~dp0%ZIP%" *)
if errorlevel 1 (popd & echo making the zip failed & goto :fail)
popd
echo.
echo   %ZIP% is ready: %~dp0%ZIP%
echo.
pause
exit /b 0

:fail
echo.
echo   RELEASE FAILED. Read the last message above.
echo.
pause
exit /b 1
