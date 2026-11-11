# Working on qui

qui is an immediate-mode UI framework. Apps are written in Go or C++ and render natively or as real DOM elements in the browser.

This file is for changing the framework. To write apps with it, read [`SKILL.md`](SKILL.md). [`README.md`](README.md) has the overview and a full repo map.

## Commands

Run these from the repo root.

| Task | Command |
| --- | --- |
| Build everything native | `go build ./...` |
| Vet | `go vet ./...` |
| Build everything for the browser | `GOOS=js GOARCH=wasm go build ./...` |
| Build the C++ libraries | `cmake -S . -B build && cmake --build build -j8` |
| Regenerate wire code | `go run ./tools/gen` |
| Run an example natively | `go run ./examples/billing` |
| Serve an example at `127.0.0.1:8080` | `go run ./examples/billing/serve` |
| Production web bundle | `go run ./cmd/qui build ./examples/billing` |
| Windows `.exe` | `go run ./cmd/qui build -target windows ./examples/billing` |
| Android `.apk` | `go run ./cmd/qui build -target android ./examples/billing` |

The examples are `showcase`, `landing` and `billing`. Native builds need Go 1.24+ and `sdl3 freetype2 harfbuzz fontconfig` through pkg-config. `sys/` also needs `gio-2.0 gio-unix-2.0 libpipewire-0.3`.

There is no test suite, so verify by running things (see [Verifying](#verifying)).

## How a frame flows

```
app code
  -> qui.go or qui.hpp encode one flat byte stream
  -> native:  frame_native.go -> src/native.go (cgo) -> src/app.cpp (window, events) -> src/engine.cpp (decode, layout, paint, damage) -> src/gl.cpp or src/cpu.cpp
  -> browser: frame_js.go -> web/qui.js (patches real DOM)
```

Input results come back to the app on the next frame as a `Response`. The format is documented in `docs/wire.md`.

## Making changes

`tools/gen/spec.txt` is the single source for the wire format. One `op` line
declares an op's payload, the node fields it sets and its docs; one `style` line
declares a `Style` or `TextStyle` field. `go run ./tools/gen` then writes:

- the Go and C++ `Style`/`TextStyle` structs, their encoders and class merges (`style_gen.go`, `src/style.gen.inc`)
- the engine's decoder into `NodeProps` (`src/node.gen.hpp`) and the browser's `decodeProps` (`web/wire.gen.js`)
- constants (`wire_gen.go`, `src/wire.gen.hpp`), the op table in `docs/wire.md`, the field lists in `SKILL.md`, and `docs/cpp.md` from `src/qui.hpp`

Never hand-edit those; `go run ./tools/gen -check` fails when one is stale. To add a style property or op:

1. Add it to `tools/gen/spec.txt` and bump `version`. If decoding needs more than storing the payload, mark it `native=custom` or `web=custom` and add a case to the decode switch in `src/engine.cpp` or `web/qui.js`. Mark a backend that ignores it `skip`, which records the parity gap.
2. Run `go run ./tools/gen`.
3. Use the new node fields: layout, paint and damage in `src/engine.cpp`, drawing in `src/gl.cpp` and `src/cpu.cpp`, CSS in `web/qui.js`.

New node kinds and `Ui` methods are still written by hand in `qui.go` and `src/qui.hpp`; run the generator afterwards to refresh `docs/cpp.md`.

Rules that are easy to break:

- **Damage:** anything that changes pixels must be visible to `Engine::swap_and_damage` in `engine.cpp`, or the frame is skipped. Shader uniforms once went stale this way.
- **New C++ files:** start them with `//go:build cgo` (in `sys/`, `//go:build linux && !android && cgo`) or the wasm build fails, keep them in `src/` (cgo only compiles C++ in the Go package folder) and add them to `CMakeLists.txt`.
- **Parity:** layout changes in `engine.cpp` usually need matching CSS in `web/qui.js`. Native and web should look the same.
- **Real DOM:** web output must stay real elements for accessibility. Never replace it with a canvas.
- **Dependencies:** library packages use only the standard library. `esbuild` and `brotli` are for `cmd/qui` only.
- **Skill size:** public API changes must update `SKILL.md`, and it should stay compact (about 4,600 words now).

## Verifying

Setting `QUI_SHOT`, `QUI_TREE`, `QUI_WIRE` or `QUI_DRIVE` runs an app offscreen and exits. `SKILL.md` lists every variable. `QUI_TREE=-` prints an outline of what is on screen with each node's `@key`, and `QUI_DRIVE` steps can aim at those keys or at text instead of coordinates; the drive code is in `src/drive.cpp` and `drive_step` in `src/app.cpp`.

Native:

- Offscreen runs use the CPU renderer, which skips shaders. Set `QUI_BACKEND=gl` to see them, and check both renderers for rendering changes. Offscreen GL uses SDL's `offscreen` video driver, so it needs no display; set `SDL_VIDEO_DRIVER` to override.
- Offscreen runs 4 frames by default, with `u_time` frozen at 1.0. Frames run faster than real time, so use `QUI_FRAMES=1500` to let animations settle.
- To patch a timestep or stub code for one run, use `go build -overlay overlay.json` with `{"Replace":{"/abs/original.go":"/abs/patched.go"}}` instead of editing source. `GOFLAGS=-overlay=overlay.json` also works through `qui build`.
- `QUI_SHOT` PNGs are uncompressed, about 5 MB at 1440x900, so don't commit them. README images are 1440x900 from `QUI_BACKEND=gl`, converted with `magick a.png -alpha off -quality 88 -define webp:method=6 a.webp`.

Browser:

- Serve the example, then run `firefox --headless --no-remote --profile <empty dir> --window-size 1440,900 --screenshot out.png http://127.0.0.1:8080/`.
- `--screenshot` does not capture WebGL. Check shaders through compile errors or `readPixels`.
- The screenshot fires on page load. To run a test script first, keep load pending with an `<img>` that your test server only answers once the script reports back.
- Buttons respond to `el.click()`, not synthetic pointer events.
- Firefox covers rendering. Touch was tested in Chromium, where CDP `Input.dispatchTouchEvent` produces real pointer events, `touch-action` and native scrolling.
- Each `Shader` uses one WebGL2 context and browsers cap them, so keep a page to about 8.

Windows and Android:

- `qui build -target windows|android` downloads pinned toolchains and libraries into `~/.cache/qui`. `-arch` picks the architecture; Android needs `ANDROID_HOME`, or an SDK in `~/Android`. Bump `depsStamp` in `cmd/qui/native.go` to force a rebuild of the libraries.
- Windows: `-console` keeps stdout, then run the `.exe` under Wine. Offscreen variables work there, with paths like `QUI_SHOT=Z:/tmp/a.png`. An empty prefix has no system fonts, so copy some into its `C:\windows\Fonts`. If Wine cannot find a GL pixel format, stderr says so and the app runs on the CPU renderer, where shaders paint only their `Bg`.
- Android: drive the emulator with `adb shell input tap` or `input swipe`, capture with `adb exec-out screencap -p`, and read stderr through `adb logcat -s qui`.
- Offscreen touch goes through `QUI_DRIVE` steps with a synthetic clock, so velocity, long press and fling are repeatable.
- The emulator's GLES silently renders shaders with `const` global arrays as zeros and reports no error. Drop the `const`.

## Linux desktop code

- **Don't touch the real desktop while verifying:** `sys.OpenScreen` and the showcase's Capture window open real portal dialogs, and `sys.Install` writes real `.desktop` files and user services. Ask first, or overlay `examples/showcase/capture_linux.go` with a fake.
- **Portal registry, measured on xdg-desktop-portal 1.22.1 and -kde 6.8.0:** `Register` takes the bare id with no `.desktop` suffix. `Exec=` must be absolute because the portal's `PATH` has no `~/.local/bin`. New `.desktop` files are seen without a restart.

## Direction

The goal is to replace the UI layers of `../litecord`, `../velocityplayer` and `../velocitycode`. Take architecture from litecord's `src/ui` and the visual language from velocitycode. It has to stay light on VRAM and work on integrated GPUs and in software.

Not built yet: Vulkan and images.

## Conventions

- Commits are conventional with no scope (`feat:`, `fix:`, `docs:`). Make one working step per commit, each one compiling. Commit as you go and push only when asked.
- Add comments only where the code is genuinely unclear.
- Don't add tests or example files unless asked.
