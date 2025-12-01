@echo off
setlocal

rem ===================================================================
rem  ESP-IDF Build, Flash, and Monitor Script
rem ===================================================================
rem
rem  This script performs the following steps:
rem  1. Builds the project using `idf.py build`.
rem  2. If the build is successful, it flashes the device using `idf.py flash`.
rem  3. If flashing is successful, it starts the serial monitor on COM8 using `idf.py -p COM8 monitor`.
rem
rem  If any step fails, the script will stop immediately.
rem ===================================================================

echo.
echo [INFO] Starting ESP-IDF build...
echo =====================================

idf.py build

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Project build failed. Halting script.
    goto:eof
)

echo.
echo [INFO] Build successful. Flashing device...
echo =============================================

idf.py flash

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Flashing failed. Halting script.
    goto:eof
)

echo.
echo [INFO] Flash successful. Starting monitor on COM8...
echo ======================================================

idf.py -p COM8 monitor

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Failed to start monitor.
    goto:eof
)

echo.
echo [INFO] Monitor closed. Script finished.
echo ==========================================

endlocal
