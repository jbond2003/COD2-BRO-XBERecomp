@echo off
rem Gets the latest changes and builds the game. Double click it, or run .\update.bat
cd /d "%~dp0"
git pull
if errorlevel 1 (
    echo.
    echo git pull failed: nothing was built. Close the game if it is open and try again.
    pause
    exit /b 1
)
call build.bat
