@echo off
docker run --rm -i -w /app -v "%CD%:/app" pebble-sdk-touch pebble build
