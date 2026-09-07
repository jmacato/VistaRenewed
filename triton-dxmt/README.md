# DXMT

A Metal-based translation layer for Direct3D 11 and 10 which allows running 3D applications on macOS using Wine.

For the current status of the project, please refer to the [project wiki](https://github.com/3Shain/dxmt/wiki).

The most recent development builds can be found [here](https://github.com/3Shain/dxmt/actions).


## Build

See [DEVELOPMENT.md](docs/DEVELOPMENT.md)

## Native backend

A native (non-Wine) macOS build links every module into a single
`libdxmt-native.dylib` that exports the standard D3D11/DXGI/D3D10 entry
points plus a small embedder API (`dxmt_native.h`): Win32-style events
(`dxmt_event_*`) and cross-process shared textures over POSIX shm. An
embedder `dlopen()`s the one dylib; `pkg-config --cflags --libs
dxmt-native` provides the include and link flags. Build it by configuring
without a cross file (see [DEVELOPMENT.md](docs/DEVELOPMENT.md)).
