# qui

One UI API for native apps and the real browser DOM. Write it once in Go or C++.

![Landing page for a made-up hosting company: a glowing dotted planet rising behind the headline "Push once. Live everywhere."](docs/images/landing.webp)

> **Agents:** read [`SKILL.md`](SKILL.md) first, then see [For agents](#for-agents).

You rebuild the whole interface every frame from your own variables. There are no widget objects, callbacks or bindings, and `Frame()` blocks until something changes, so an idle app uses no CPU.

```go
package main

import (
	"strconv"

	"github.com/torisetxd/qui"
)

func main() {
	ui := qui.Open("Counter", 480, 320)
	defer ui.Close()
	count := 0
	for ui.Frame() {
		ui.Column(qui.Style{W: qui.Fill(), H: qui.Fill(), Pad: qui.All(16), Gap: 8}, func() {
			ui.Text("Counter", qui.H1)
			ui.Text(strconv.Itoa(count))
			if ui.Button("Add").Clicked {
				count++
			}
		})
	}
}
```

`go run .` opens a native window. Build the same file with `GOOS=js GOARCH=wasm` and it renders real HTML elements in the browser.

C++ is the same API in `snake_case`:

```cpp
#include "qui.hpp"
#include <string>
using namespace qui;

int main() {
  Ui ui("Counter", 480, 320);
  int count = 0;
  while (ui.frame()) {
    ui.column({.w = fill(), .h = fill(), .pad = 16, .gap = 8}, [&] {
      ui.text("Counter", H1);
      ui.text(std::to_string(count));
      if (ui.button("Add").clicked) ++count;
    });
  }
}
```

## Examples

Each one is a plain qui app, shown here on the native GPU renderer. The companies and numbers are made up.

| Showcase | Billing dashboard |
| --- | --- |
| ![A desktop of floating windows: live shader editor, million-row log, performance monitor, particle swarm and chat](docs/images/showcase.webp) | ![Dark billing dashboard with a gross volume chart, balances and report cards](docs/images/billing.webp) |

| Landing page: deploy section | Landing page: network section |
| --- | --- |
| ![Deploy steps next to a live terminal log](docs/images/deploy.webp) | ![Draggable glowing globe next to a list of city latencies](docs/images/network.webp) |

Run them from the repo root:

| App | Native window | Browser, at `127.0.0.1:8080` |
| --- | --- | --- |
| Showcase | `go run ./examples/showcase` | `go run ./examples/showcase/serve` |
| Landing page | `go run ./examples/landing` | `go run ./examples/landing/serve` |
| Billing dashboard | `go run ./examples/billing` | `go run ./examples/billing/serve` |

## Features

- **Layout:** a strict flexbox subset (`Column`, `Row`, `Stack`) that maps directly to CSS.
- **Text:** HarfBuzz shaping and mixed-direction text. Inputs have selection, undo, clipboard and IME.
- **Virtual lists:** `List` builds only the visible rows, and row heights can vary.
- **Shaders:** write one GLSL function and place it like any other box.
- **Styling:** gradients on boxes and text, box shadows, letter spacing and four built-in themes.
- **Gestures:** pan with axis arbitration against scrollers, fling scrolling, pinch, long press, double tap and a swipeable drawer.
- **Renderers:** OpenGL ES 3.0 or desktop GL 3.3, with a CPU rasterizer as the fallback.
- **Real DOM on the web:** actual buttons, inputs and headings, with roles and labels for screen readers.
- **Linux desktop tools:** `qui/sys` handles `.desktop` install, background services, and screen and microphone capture with saved permissions.

## Status

| Target | State |
| --- | --- |
| Linux native | Works |
| Browser (Go wasm) | Works, tested in Firefox |
| Windows | Works, tested under Wine |
| Android | Works, tested on the x86_64 emulator; arm64 builds but has not been run |

Also missing: animated images, right-to-left paragraph direction and global shortcuts. Android APKs are debug-signed, and `qui build` only packages Go apps for Android.

## Getting started

You need Go 1.24 or newer. Native builds also need `sdl3 freetype2 harfbuzz fontconfig` through pkg-config, and `qui/sys` needs `gio-2.0 gio-unix-2.0 libpipewire-0.3`.

Clone qui next to your app:

```sh
git clone https://github.com/torisetxd/qui ../qui
```

**Go:** add this to `go.mod`:

```
require github.com/torisetxd/qui v0.0.0

replace github.com/torisetxd/qui => ../qui
```

**C++:** needs CMake 3.25 and C++20. With no checkout, fetch it:

```cmake
include(FetchContent)
FetchContent_Declare(qui GIT_REPOSITORY https://github.com/torisetxd/qui GIT_TAG main)
FetchContent_MakeAvailable(qui)
target_link_libraries(app PRIVATE qui::qui)
```

Or check it out as a subfolder (or git submodule) and use `add_subdirectory(qui)`. `#include "qui.hpp"` then works with no extra include paths. For the Linux desktop tools, also link `qui::sys` and include `qui_sys.hpp` (set `-DQUI_BUILD_SYS=OFF` to skip it).

### Ship to the web

Install the CLI once from this repo, then build any qui app into a static folder:

```sh
go install ./cmd/qui
qui build ./myapp -title "My app" -bg "#000"
```

That writes `myapp/dist` with a stripped wasm, one minified JS bundle, hashed filenames and `.br` and `.gz` copies. Add an `index.html` to your app folder to replace the generated page, and a `public/` folder for favicons and fonts. Your host must serve `.wasm` as `application/wasm`.

### Ship to Windows and Android

From Linux, with no system packages beyond Go and CMake:

```sh
qui build -target windows ./myapp
qui build -target android -id com.example.myapp ./myapp
```

The first build downloads pinned toolchains and libraries into `~/.cache/qui`. Windows needs nothing else and produces one static `.exe`. Android also needs a JDK and the SDK and NDK, in `ANDROID_HOME` or `~/Android`, and produces a debug-signed `.apk`. Output goes to `myapp/dist/<target>`.

## How it works

```
your app (Go or C++)
   |  one flat byte stream per frame
   |
   |-- native   engine: layout, text, paint  ->  OpenGL or CPU  ->  SDL window
   `-- browser  web/qui.js: patches real DOM elements
```

Both sides read the same stream, so a new language only needs an encoder. Input comes back as a `Response` on the next frame. The format is documented in [`docs/wire.md`](docs/wire.md).

## For agents

[`SKILL.md`](SKILL.md) covers the whole API in about 4,000 tokens: the rules that cause most mistakes, every style field, build commands and what doesn't exist yet. To install it as a skill, copy it to `~/.claude/skills/qui/SKILL.md` or your tool's equivalent.

Check your work without opening a window. These run from the repo root and exit on their own:

| Goal | Command |
| --- | --- |
| Screenshot | `QUI_SHOT=a.png QUI_SIZE=1440x900 go run ./examples/billing` |
| Screenshot with shaders | `QUI_BACKEND=gl QUI_FRAMES=1500 QUI_SHOT=a.png go run ./examples/billing` |
| Every node's rect and text | `QUI_TREE=a.json go run ./examples/billing` |
| Click, then screenshot | `QUI_DRIVE='click:116,152' QUI_SHOT=a.png go run ./examples/billing` |

- Offscreen runs use the CPU renderer unless you set `QUI_BACKEND=gl`, and the CPU renderer skips shaders.
- Frames run faster than real time offscreen, so raise `QUI_FRAMES` if an animation hasn't settled.
- `SKILL.md` lists every scripted input step, including typing, dragging and scrolling.

When changing the framework itself:

- To add an op, style field, node kind or token, edit `tools/gen/spec.txt` and run `go run ./tools/gen`. It writes the style structs and encoders for Go and C++, both decoders and the docs; never hand-edit its output (`*_gen.go`, `src/*.gen.*`, `web/wire.gen.js`). `AGENTS.md` has the steps.
- Rendering changes need checking on `QUI_BACKEND=gl`, `QUI_BACKEND=cpu` and in a browser.

### Repo map

| Path | What it is |
| --- | --- |
| `qui.go`, `frame_native.go`, `frame_js.go` | Go API and its native and wasm frame loops |
| `src/` | Native C++ engine and its cgo bridge; the Go package `qui/src` is internal plumbing |
| `src/qui.hpp`, `src/qui_c.h` | C++ API and the C ABI |
| `src/engine.cpp` | Decodes frames, runs layout, builds paint lists and damage |
| `src/app.cpp`, `src/input.cpp` | Window, events, offscreen mode and text editing |
| `src/gl.cpp`, `src/cpu.cpp` | GPU and CPU renderers |
| `src/font_manager.cpp`, `src/text_shaper.cpp` | Fonts, glyph cache and shaping |
| `web/qui.js` | Browser DOM patcher |
| `tools/gen` (`spec.txt`) | Wire format and style source, and the code generator |
| `docs/cpp.md` | The C++ API without function bodies, generated from `src/qui.hpp` |
| `cmd/qui` | The `qui build` command |
| `sys/` | Linux install, services and capture |
| `examples/` | Showcase, landing page and billing dashboard |
