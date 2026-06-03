@echo off
setlocal EnableDelayedExpansion

set INITIAL_DELAY=0
set COUNT=1
set BETWEEN_DELAY=0

if not "%1"=="" set INITIAL_DELAY=%1
if not "%2"=="" set COUNT=%2
if not "%3"=="" set BETWEEN_DELAY=%3

if !INITIAL_DELAY! gtr 0 (
    echo Waiting !INITIAL_DELAY!s before starting...
    set /a _p=!INITIAL_DELAY!+1
    ping -n !_p! 127.0.0.1 >nul
)

set /a i=1
:loop
echo Taking screenshot !i! of !COUNT!...
docker run --rm -i -w /app -v "%CD%:/app" pebble-sdk-touch pebble screenshot --phone 192.168.1.108

if !i! lss !COUNT! (
    if !BETWEEN_DELAY! gtr 0 (
        echo Waiting !BETWEEN_DELAY!s...
        set /a _p=!BETWEEN_DELAY!+1
        ping -n !_p! 127.0.0.1 >nul
    )
    set /a i+=1
    goto loop
)

echo Done.
endlocal
