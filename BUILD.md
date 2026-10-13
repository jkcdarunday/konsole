# Konsole - KDE's Terminal Emulator

Konsole is a terminal program for KDE.


## HOWTO Build

1. Install dependencies. On neon:
```
apt install git cmake make g++ extra-cmake-modules libkf6config-dev libkf6auth-dev libkf6package-dev libkf6declarative-dev libkf6coreaddons-dev libkf6kcmutils-dev libkf6i18n-dev libkf6crash-dev libkf6newstuff-dev libkf6textwidgets-dev libkf6iconthemes-dev libkf6dbusaddons-dev libkf6notifyconfig-dev libkf6pty-dev libkf6notifications-dev libkf6parts-dev qt6-base-dev libqt6core6t64 libqt6widgets6 libqt6gui6 libqt6qml6 qt6-multimedia-dev libicu-dev
```
2. Clone with `git clone https://invent.kde.org/utilities/konsole.git`
3. Make _build_ directory: `mkdir konsole/build`
4. Change into _build_ directory: `cd konsole/build`
5. Configure: `cmake ..` (or `cmake .. -DCMAKE_INSTALL_PREFIX=/where/your/want/to/install`)
6. Build: `make`
7. Install: `make install`

## GPU rendering

The standalone application selects Vulkan when Qt 6.7 or newer with Qt Quick
and Qt Quick Widgets (including matching private development headers) is available
and a Vulkan device can be created. Text uses
Qt's GPU glyph atlases, and unchanged screen strips remain in the scene graph.
Older Qt builds and systems without Vulkan use OpenGL. Rendering falls back to
software if the OpenGL surface cannot initialize. Embedded Konsole parts respect
the host application's graphics backend.

Output updates follow the fastest visible monitor's refresh rate and adapt when
the window moves between monitors. Presentation remains controlled by Qt and the
desktop compositor; idle terminals do not redraw continuously.

Run the built application with `build-release/bin/konsole --separate`.
Use `KONSOLE_RENDERER=opengl` to select OpenGL, or `KONSOLE_RENDERER=software`
to select the raster renderer. An explicit `QSG_RHI_BACKEND` overrides the default
Qt Quick backend selection. Enable renderer diagnostics with
`QT_LOGGING_RULES='org.kde.konsole.debug=true;qt.rhi.general=true'`.
Use `KONSOLE_RENDER_STATS=1` for scene update rates and cached-row diagnostics.
Configure with `-DWITH_VULKAN_RENDERER=OFF` to build only the OpenGL/software paths.
The extra Qt Quick batching delay is removed only when the runtime Qt version
matches the version used to build Konsole.

`TerminalRenderingTest` checks Vulkan drawing and reports frame rendering rates
on a desktop display. It skips presentation checks with headless Qt plugins.
Set `KONSOLE_GPU_TEST_HIDDEN=1` to render full-screen-sized framebuffers without
showing test windows, and `KONSOLE_GPU_TEST_PROFILE=Konsole.profile` to benchmark
an existing profile. Hidden rendering measures GPU frame generation, not scanout.
For a visual check, run:
```
build-release/bin/konsole --separate -e python3 tests/features/gpu-render-demo.py --fps 144 --duration 10
```
The demo exits after ten seconds. Its input rate is not a measurement of physical
presentation rate.
