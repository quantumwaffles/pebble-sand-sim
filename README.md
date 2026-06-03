# Pebble Sand Sim

A falling-sand simulation game for Pebble smartwatches, targeting the
**Emery** platform (Pebble Time 2) for its color display and touch screen.

This is an early skeleton — right now the app just draws "Hello" centered on
the watch face. The simulation is built up from here.

## Supported platforms

`emery` only, for now. Emery has the color display and capacitive touch the
game is designed around.

## Building

This app uses the current Core Devices PebbleOS SDK (4.9+), which is needed for
the TouchService API. The repo ships its own build image.

Build the SDK image once:

```sh
docker build -t pebble-sdk-touch .
```

Then build the app:

```sh
docker run --rm -i -w /app -v "$PWD:/app" pebble-sdk-touch pebble build
# or on Windows:
build.cmd
```

If you have the Pebble SDK installed natively instead, `pebble build` works
directly (Python 3.10–3.13 + `uv tool install pebble-tool`, then
`pebble sdk install latest`).

## Installing

Enable the developer connection in the Pebble phone app to get its IP, then:

```sh
docker run --rm -i -w /app -v "$PWD:/app" pebble-sdk-touch pebble install --phone <phone-ip>
# or on Windows (caches the IP in .pebble-ip):
install.cmd <phone-ip>
```

## Layout

- `src/c/main.c` — the app (currently the Hello skeleton).
- `src/pkjs/app.js` — PebbleKit JS side (no config yet).
- `Dockerfile` — touch-capable build environment (`pebble-sdk-touch`).
- `build.cmd` / `install.cmd` / `screenshot.cmd` — Windows helper scripts.
