@echo off
set IP=%1

if "%IP%"=="" (
    if exist .pebble-ip (
        set /p IP=<.pebble-ip
    ) else (
        echo Error: No phone IP provided and no cached IP found.
        echo Usage: install ^<phone-ip^>
        echo Example: install 192.168.1.42
        exit /b 1
    )
)

echo %IP%>.pebble-ip
echo Installing to %IP%...
docker run --rm -i -w /app -v "%CD%:/app" pebble-sdk-touch pebble install --phone %IP%
