---
name: qui
description: Build, change or verify UIs with qui, the in-house immediate-mode framework (Go or C++; native through OpenGL ES or a CPU rasterizer, browser through real DOM elements and Go wasm), and its qui/sys module for Linux app install, background services and screen or microphone capture. Use when a project imports github.com/torisetxd/qui or includes qui.hpp, or when asked to add a screen, widget, list or shader to a qui app, or to install a native app or capture the screen or mic with it.
---

# qui

Immediate-mode API over retained internals. Rebuild the whole UI every frame from your own state: no widget objects, callbacks or bindings. `Frame()` blocks until something changes, so an idle app uses no CPU.

## Minimal app

Go. The same source builds for native and wasm.

```go
package main

import (
	"strconv"

	"github.com/torisetxd/qui"
)

func main() {
	qui.SetAppID("dev.example.counter")
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

C++. Names are snake_case; everything else matches.

```cpp
#include "qui.hpp"
#include <string>
using namespace qui;

int main() {
  set_app_id("dev.example.counter");
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

## Rules that bite

- **Responses come from the previous frame.** `Clicked` is true exactly once, on the frame after the input. The engine schedules that frame for you.
- **Never block the loop.** Do I/O on a goroutine or thread, guard the shared state yourself, then call `ui.Wake()` (safe from any thread) to request a frame.
- **Go: call everything from the main goroutine**, except `Wake`.
- **Identity is parent id + `Key`, or sibling index without one.** Buttons key on their label, so same-label siblings need `Key`. Give `Key` to scrollers, lists and `Hit` boxes whose earlier siblings come and go, or their state moves to another node.
- **Boxes report nothing without `Hit: true`.** Buttons and lists always report.
- **Zero means unset** in `Style` and `TextStyle`: `Opacity: 0` is opaque, `Bg: 0` paints nothing.
- **C++ designated initializers must follow declaration order** (the field list below).
- **Wrap the root in `W: Fill(), H: Fill()`**; the default is fit-to-content.

## Layout

A strict flexbox subset that maps directly to CSS.

- `Column`, `Row`, and `Stack` (children overlap). `Absolute: true, X, Y` positions relative to the parent; `Fixed: true, X, Y` relative to the window, where `Fill` means the window size and the parent's clip and scroll do not apply.
- Sizes: fit by default, `Px(n)`, `Fill()`, `FillN(weight)`, `Pct(n)`.
- Padding: Go `All(n)`, `XY(x, y)`, `Edges{Top, Right, Bottom, Left}`; C++ `16`, `{v, h}`, `{t, r, b, l}`.
- `Align` is the cross axis (`Start Center End Stretch`), `Justify` the main axis (`Start Center End Between`).
- Long text inside a `Row` needs `W: Fill()` to wrap instead of overflow.
- `Wrap: true` on a `Row` flows children onto new lines (tags, chips, reactions); `Gap` spaces both ways and `Justify`/`Align` apply per line.
- Button labels are centered. `Justify: Between` left-aligns one, `Justify: End` right-aligns it.
- `Scroll: true` scrolls vertically and needs a bounded height. Use `List` for long content.
- `Z` sets paint and hit order among siblings. Use it to raise overlapping panels: unlike reordering children, it keeps their scroll position and focus on the web. Give overlapping panels `Hit: true` so the pointer and wheel stop at them.

- `Gradient: qui.Gradient{From, To, Angle}` fills a box; on `TextStyle` it colors the letters across the text box (keep such text fit-width). Angles are CSS degrees. `Shadow: qui.BoxShadow{Color, Blur, Y}` paints outside the box, so a bright color with no `Y` is a glow. `TextStyle.Tracking` is letter spacing in px; large headings look best around `-0.04 * Size`.
- Text is mouse-selectable everywhere, as in a browser (drag, double-click a word, triple-click a text, Ctrl+C copies), except in buttons, inputs, menus, switches and sliders; a drag that selects text does not click or drag the `Hit` box under it. `NoSelect: true` turns selection off for a subtree (navigation, lists you click through, draggable tiles) and `Select: true` turns it back on below and keeps a selection that starts there inside it; modals, popups and drawers do that already. `ui.SelectedText()` (C++ `selected_text()`) returns the selection for a Copy menu item. Touch input does not select.
- `ScrollTo: &offset` sets a `Scroll` box's offset for that frame. Animate it yourself for smooth anchors, and stop sending it when done so the user can scroll. A zero-height `Hit` box inside the scroller reports its viewport-relative `Y` even offscreen, which gives you scroll position, reveal-on-scroll and scroll-spy. Web scrolling does not wake the loop, so run a ticker calling `ui.Wake()` for those.

`Style` fields, in order: `W H MinW MaxW MinH MaxH Pad Gap Align Justify Absolute X Y Z Bg Fg Border BorderW Radius Opacity Gradient Shadow Clip Scroll Hit ScrollTo Gestures Role Label Key Class Select Wrap NoSelect Ease HoverBg Drag Fixed NoScrollbar` (C++ `select`, `wrap`, `no_select`, `ease`, `hover_bg`, `drag`, `fixed`, `no_scrollbar` come last)

`TextStyle` fields, in order: `Size Bold Mono Ellipsis Color W Align Role Gradient Tracking Class`. Presets: `Body H1 H2 Small Code`.

`Response` fields: `Found Hovered Pressed Clicked RightClicked Focused DragStart Dragging DragEnd Changed Submitted LongPressed DoubleTapped PanStart Panning PanEnd Pinching X Y W H DX DY VX VY Scale Swipe First Count Error`. `ui.Peek(key)` returns last frame's `Response` for a keyed node before you build it, to style it by its hover (C++ `peek`).

Frame info: `Width Height Dpr Dt PointerX PointerY PointerDown PointerTouch` (a lifted finger keeps its position, so skip hover effects while `PointerTouch && !PointerDown`). `Stats()` gives `Nodes WireBytes EngineMs` for the last frame.

## Color and semantics

Prefer theme tokens, which track `ui.SetTheme(qui.Midnight)` (`Slate` default, `Midnight`, `Warm`, `Neutral`):

`Background Surface Raised Inset Ink Muted Strong Accent Accent2 Hover Active Item Line Danger Selection Scrollbar ScrollbarHot Scrim Shadow`

Custom themes and app tokens: `qui.Token(20)` and up are yours. `ui.SetPalette(qui.Midnight, map[qui.Color]qui.Color{Rail: qui.RGB(..), qui.Accent: ..})` picks a base theme and overrides or adds tokens, so every box using them re-themes at once (C++: `ui.set_palette(base, {{Ui::token(20), rgba(..)}})`).

Classes: `ui.Class("card", qui.Style{Bg: Panel, Pad: qui.All(12), Radius: 8})` then `Style{Class: "card elevated"}`. Classes fill only fields left at zero; later classes beat earlier ones and the element's own fields beat both. `ui.TextClass` and `TextStyle.Class` do the same for text (C++: `define_class`, `define_text_class`, `.klass`, which comes first in `Style` and `TextStyle` so it leads a designated initializer). They are resolved in your process, so they cost nothing on the wire.

Literals: `qui.RGB(r, g, b)`, `qui.RGBA(r, g, b, a)`; C++ `rgba(r, g, b, a)`.

On the web, `Role` picks the element (`RoleHeading` h2, `RoleLink` a, `RoleMain` main, `RoleNav` nav, `RoleImage` role=img) and `Label` sets the accessible name.

Controls: `if ui.Switch("sounds", on, "Sounds") { on = !on }` and `vol = ui.Slider("volume", vol, qui.Px(160), "Volume")` (0 to 1). Both take Tab focus, work from the keyboard (Space, arrows, Home, End) and are role=switch / role=slider on the web. C++: `ui.toggle_switch(key, on, label)`, `ui.slider(key, value, w, label)`.

Context menus: keep a `qui.Menu` in app state, call `a.menu.Show(ui.PointerX(), ui.PointerY())` on a right click, and each frame `switch ui.Menu(&a.menu, []qui.MenuItem{{Label: "Copy"}, {Label: "Delete", Danger: true, Confirm: "Press again"}}) { case 0: ... }` (-1 when nothing was chosen). It stays on screen, takes the arrows, Home, End, Enter and Space, closes on Escape or a press outside, and hands focus back. Opened by touch on a window under 600 wide, it shows as a full-width `Sheet`. `ui.MenuWithHeader(&a.menu, items, header)` draws `header` above the rows, such as quick actions. C++: `ui.menu(m, items, panel_style)` or `ui.menu(m, items, panel_style, header)`, `m.show(x, y)`.

## Text input and keys

```go
package main

import "github.com/torisetxd/qui"

func main() {
	ui := qui.Open("Chat", 480, 320)
	defer ui.Close()
	var draft, last string
	for ui.Frame() {
		if ui.KeyPressed('k', qui.ModCtrl) {
			draft = ""
		}
		ui.Column(qui.Style{W: qui.Fill(), H: qui.Fill(), Pad: qui.All(16), Gap: 8}, func() {
			ui.Text(last)
			field := ui.Input("draft", &draft, qui.InputOptions{
				Style:        qui.Style{W: qui.Fill(), MaxH: 120},
				Placeholder:  "Message",
				Multiline:    true,
				EnterSubmits: true,
				Autofocus:    true,
			})
			if field.Submitted {
				last, draft = draft, ""
			}
		})
	}
}
```

`Input` takes a stable key and a pointer to your string, which it updates when `Changed` is true. Assigning to the string yourself replaces the field and moves the caret to the end. Options: `Style Placeholder Size Mono Multiline EnterSubmits Password Autofocus Focus PassthroughNav Bare` (`Autofocus` takes focus each time the field appears; `Focus` takes focus on every frame it is true; `Bare` drops the field's background, border and focus accent so it can sit inside your own box, which can show focus from `Response.Focused`); C++ `input(key, value, {.style, .placeholder, .size, .mono, .multiline, .enter_submits, .password, .autofocus, .focus, .passthrough_nav, .bare})`.

Enter submits a single-line field. In a multiline field Enter adds a newline and Ctrl+Enter submits, unless `EnterSubmits` is set, where Enter submits and Shift+Enter adds a newline. Multiline fields grow with their text up to `MaxH`, then scroll. Native fields handle caret movement, selection, clipboard, undo, IME composition and Linux primary selection; the web uses real `<input>` and `<textarea>`.

`ui.KeyPressed(key, mods...)` (C++ `ui.key_pressed(key, mods)`) needs the exact modifiers. Keys are `KeyEnter KeyEscape KeyBackspace KeyDelete KeySpace KeyLeft KeyRight KeyUp KeyDown KeyHome KeyEnd KeyPageUp KeyPageDown KeyF1..KeyF12 KeyBack KeyForward` (the mouse back and forward buttons) or a lowercase rune. Mods are `ModShift ModCtrl ModAlt ModMeta`. A focused field keeps the keys it uses, Tab moves focus, and Enter or Space clicks a focused button.

## Virtual list

```go
package main

import (
	"strconv"

	"github.com/torisetxd/qui"
)

func main() {
	ui := qui.Open("List", 480, 640)
	defer ui.Close()
	for ui.Frame() {
		ui.List("rows", 100000, 34, qui.ListOptions{Style: qui.Style{W: qui.Fill(), H: qui.Fill()}}, func(i int) {
			ui.Text("Row "+strconv.Itoa(i), qui.TextStyle{W: qui.Fill()})
		})
	}
}
```

Arguments are key, row count, estimated row height, options, row body. `JumpTo` scrolls an item to the top for one frame; with `Glide: true` it eases the item to the middle instead, and a wheel or drag cancels it (native only). The body runs only for visible rows and heights may vary. To load older items on scroll, insert them at the front of your data and set `ListOptions.Prepend` to how many you inserted that frame (C++ `.prepend`), so the rows on screen stay put; it is native only. `Response.First` tells you when row 0 is on screen. The list needs a bounded height. `Bottom: true` starts at the end and stays pinned there, and a short list sits at the bottom, for chat logs.

## Shaders

```go
package main

import "github.com/torisetxd/qui"

const glow = `uniform float u_speed;
vec4 shade(vec2 uv) {
  return vec4(uv, 0.5 + 0.5 * sin(u_time * u_speed), 1.0);
}`

func main() {
	ui := qui.Open("Shader", 480, 320)
	defer ui.Close()
	for ui.Frame() {
		ui.Shader(qui.ShaderOptions{
			Src:      glow,
			Style:    qui.Style{W: qui.Fill(), H: qui.Fill(), Bg: qui.Raised},
			Fps:      24,
			Uniforms: []qui.Uniform{{Name: "u_speed", Values: []float32{1}}},
		})
	}
}
```

`Src` is GLSL ES 3.00 without `#version` or `precision`, and must define `vec4 shade(vec2 uv)`, with uv in 0..1 from the top-left. `u_time` and `u_resolution` are already declared; declare your own uniforms. `Fps` repaints without running your loop. The CPU backend does not run shaders and paints `Bg` instead, so always set one. A shader that fails to compile also paints `Bg`, and `ui.Shader` returns the compiler log in `Response.Error`, with line numbers relative to `Src`. Declare lookup tables as plain `float A[64]=float[64](...)`, not `const`, which renders as zeros on the Android emulator.

## Images

```go
if !ui.HasImage("avatar:42") {
	ui.LoadImage("avatar:42", 128, 128, rgba) // straight RGBA, once
}
ui.Image("avatar:42", qui.Style{W: qui.Px(40), H: qui.Px(40), Radius: 20, Clip: true})
```

`LoadImage` sends pixels once under your key, so load them off the render path as they arrive (decode on a thread, then `Wake`). `Image(key, style, fit)` refers to it by key; `FitCover` (default) crops, `FitContain` letterboxes, `FitStretch` distorts; add `| FitPixelated` or `| FitBicubic` to change how it upscales (bilinear by default). With no size it uses the pixel size, with only `W` it keeps the aspect ratio. Radius rounds the corners and a square with `Radius = W/2` is a circle. The engine resamples to the on-screen size and keeps its own copy, so load images near the size you show them. On the web it is a `<canvas>`. C++: `ui.load_image(key, w, h, rgba)`, `ui.has_image(key)`, `ui.image(key, style, fit)`.

## Icons

```go
var send = qui.NewSvg(`<svg viewBox="0 0 24 24"><path d="M3 20v-6l8-2-8-2V4l19 8z"/></svg>`)

ui.Icon(send, qui.Style{W: qui.Px(18), Fg: qui.Muted})
if ui.IconButton(send, "Send", qui.Style{}).Clicked { ... }
```

`NewSvg` once (a package var), then `Icon` each frame: the SVG text goes to the engine the first time and later frames send its hash. It draws one color, `Fg` (`Ink` by default), in a 20px box unless sized (only `W` makes it square); the document's own colors are ignored. Supported: `path` (all commands, `evenodd`), `circle`, `ellipse`, `rect` (with `rx`/`ry`), `line`, `polyline`, `polygon`, `g`, `transform`, `fill="none"`, strokes with width, caps and joins, opacity, and `style="..."` for those properties; gradients, `use`, clip paths, masks and text are skipped. Natively it is rasterized by FreeType at the exact pixel size and cached in the atlas, so it stays sharp at any scale; on the web it is a CSS mask. A `Label` makes it role=img, otherwise it is hidden from assistive tech. `IconButton(svg, label, style)` is a 32px focusable button (role=button) with a 20px icon, Muted and Ink on hover unless `Fg` is set; `Role: qui.RoleButton` on any `Hit` box gives the same keyboard, hover and accessibility behaviour to a custom button such as icon plus text. C++: `constexpr qui::Svg send{R"(<svg ...>)"};`, `ui.icon(send, style)`, `ui.icon_button(send, label, style)`.

## Rich text

```go
r := ui.Rich(text, []qui.Span{
	{Start: 6, End: 10, Flags: qui.SpanBold},
	{Start: 15, End: 19, Flags: qui.SpanMono, Bg: Code},
	{Start: 25, End: 31, Flags: qui.SpanUnderline, Fg: qui.Accent2, Link: "docs"},
}, qui.TextStyle{W: qui.Fill()})
if r.Clicked == "docs" { open() }
```

One paragraph that wraps across spans. Offsets are UTF-8 byte offsets; spans are sorted and must not overlap, and gaps are plain text. Flags: `SpanBold SpanMono SpanItalic SpanUnderline SpanStrike SpanHoverUnderline` (italic is a synthetic slant of the regular or bold face). An inline image (a custom emoji) is a span whose bytes are `qui.ImagePlaceholder` with `Image: key` set to a loaded image (C++ `kImagePlaceholder`, `.image`); it takes about 1.5em of width and draws at line height. `Link` makes a span clickable and `Hovered`, `Clicked` and `RightClicked` name it, one frame late like any response. C++: `ui.rich(text, spans, style)` with `Span{.start, .end, .flags, .fg, .bg, .link}`. On the web it is real `<span>` and `<a>` elements.

## Overlays

Call these after the content they cover. `Popup` and `Tooltip` use window coordinates from anywhere; `Modal` dims and centers in its parent. `Modal` and `Popup` return true when the user clicked outside or pressed Escape, so you can close them.

```go
if modal && ui.Modal("settings", qui.Style{W: qui.Px(420), Bg: qui.Surface, Radius: 10, Pad: qui.All(16)}, body) {
	modal = false
}
if menu && ui.Popup("ctx", x, y, qui.Style{W: qui.Px(180), Bg: qui.Raised, Radius: 8}, items) {
	menu = false
}
ui.Tooltip(row.Hovered, "Copy link")
```

`PickFiles(multiple)` opens the system file picker without blocking and `PickedFiles()` returns the paths chosen so far, clearing them; the app is woken when the user picks. Files dropped on the window arrive the same way, as do files or an image pasted with Ctrl+V into a focused input (the image is saved to a temp file that is removed on exit; Ctrl+Shift+V pastes text instead), and `FilesHovering()` is true while they are dragged over it (native only; C++ `pick_files`, `picked_files`, `files_hovering`).

`SetClipboard(text)` (C++ `set_clipboard`) puts text on the system clipboard, which a context menu's "Copy" needs; browsers only allow it inside a click.

Custom title bar: call `qui.SetBorderless(true)` before `Open` (C++ `qui::set_borderless(true)` before the `Ui`), give the bar `Drag: true` so pressing it moves the window (hit boxes inside it still click), and draw buttons that call `MinimizeWindow()`, `ToggleMaximized()` and `CloseWindow()`; `WindowMaximized()` picks the icon. The window edges still resize it. Desktop native only. C++: `.drag`, `minimize_window`, `toggle_maximized`, `close_window`, `window_maximized`.

`SetSmoothScroll(true)` eases mouse wheel steps (native; browsers smooth the wheel themselves) and `SetReduceMotion(true)` makes drawers, `Ease` and `Tween` snap. C++: `set_smooth_scroll`, `set_reduce_motion`.

Motion: `Ease: 150` on a box animates its `Bg`, `Fg`, `Border`, `Radius`, `Opacity` and button hover tint over 150ms when they change; give it a stable `Key`. `HoverBg` replaces `Bg` while the pointer is over the box or its children, with no frame of delay, so a row stays lit over its own buttons. For anything else, `v := ui.Tween(key, target, ms)` returns an eased value each frame (once per key per frame) and keeps frames coming until it arrives, e.g. a growing `H: qui.Px(v)`. `TweenFrom(key, from, target, ms)` starts at `from` the first frame the key appears, for enter animations; Modal and Popup use it to fade in. Opacity 0 means unset, so start from a small value. C++: `.ease`, `.hover_bg`, `ui.tween(key, target, ms)`, `ui.tween_from(key, from, target, ms)`.

`Modal` centers a panel over a scrim (capped to the window minus 16px a side), `Popup` places a panel at window coordinates (take them from `PointerX/PointerY` on a right click) and keeps it on screen when it has a `W`, `Sheet` slides a panel up from the bottom over a scrim and also closes on a swipe down, and `Tooltip` follows the pointer and flips to the other side of it near a window edge. C++: `ui.modal(key, style, body)`, `ui.popup(key, x, y, style, body)`, `ui.sheet(key, style, body)`, `ui.tooltip(show, text)`.

## Gestures

Touch taps, scrolling and flings work without any setup. For the rest, set `Gestures` on a box. It does not need `Hit`, and text under it stays selectable.

```go
r := ui.Row(qui.Style{Key: key, W: qui.Fill(), Gestures: qui.GesturePanX}, rowBody)
if r.Panning || r.PanEnd {
	offset += r.DX
}
if r.Swipe == qui.SwipeLeft {
	archive(i)
}
```

- `GesturePanX`, `GesturePanY`, `GesturePinch`; add `GestureTouchOnly` to ignore the mouse. Read `PanStart Panning PanEnd DX DY VX VY Swipe`, and `Pinching Scale`, where `zoom *= r.Scale` is always safe and `PointerX/Y` is the pinch center.
- The innermost box that accepts the press's dominant axis wins, and a scroller that can still move counts on the vertical axis. A `Hit` box inside a drawer or scroller only gets touch `Dragging` if nothing claims the axis, so give sliders and reorder handles their own `Gestures`.
- Any `Hit` box reports `DoubleTapped`, and `LongPressed` for touch. A long press also sets `RightClicked`, so one handler covers context menus.

`Drawer` is a panel that slides over content. Keep the state in your app. A touch swipe on the content opens it; a swipe, a tap on the scrim or Escape (Back on Android) closes it. A mouse cannot swipe it, so also open it with a button.

```go
ui.Drawer(&a.nav, qui.DrawerOptions{Side: qui.SideLeft, Size: 280, Style: qui.Style{Bg: qui.Surface, Pad: qui.All(12)}}, a.content, a.menu)
if ui.Button("Menu").Clicked {
	a.nav.Open = true
}
```

C++: `Style.gestures`, `Response.pan_end`, `ui.drawer(state, {.side = Side::Left}, content, panel)`.

On Android, Back arrives as `KeyEscape`. If no call to `KeyPressed(KeyEscape)` returns true that frame, the app goes to the background, so only check it while you have something to close.

## Build

Native needs `sdl3 freetype2 harfbuzz fontconfig` through pkg-config.

- Go: `go build .`, with `replace github.com/torisetxd/qui => ../qui` in `go.mod`.
- C++: `add_subdirectory(qui)` and `target_link_libraries(app PRIVATE qui)`.
- Web: `GOOS=js GOARCH=wasm go build -o app.wasm .`, then serve it next to `web/qui.js`, `web/wire.gen.js` and `$(go env GOROOT)/lib/wasm/wasm_exec.js`:

```html
<!doctype html>
<style>html, body { height: 100%; margin: 0 }</style>
<script src="wasm_exec.js"></script>
<script type="module">
  import { start } from "./qui.js";
  await start({ wasm: "app.wasm", root: document.body, preventKeys: ["ctrl+k"] });
</script>
```

For production, install the CLI once with `go install ./cmd/qui` from the qui repo, then run `qui build [app folder]`. It writes `dist/` with a stripped wasm, one minified JS bundle, content-hashed names and `.br` and `.gz` copies. Flags go before or after the folder: `-o out -title T -bg '#000' -sans F -mono F -prevent-keys ctrl+k,ctrl+s -compress=false`.

- An `index.html` in the app folder replaces the generated page. The script tag is added for you; keep `html, body { height: 100%; margin: 0 }` in it.
- `public/` in the app folder is copied into the output as is, for favicons, fonts and `robots.txt`.
- The host must serve `.wasm` as `application/wasm`, and needs configuring to use the `.br` and `.gz` copies. Hashed files can be cached forever; `index.html` should not be.

`qui build -target windows [folder]` makes one static `.exe`, and `-target android -id com.example.app [folder]` a debug-signed `.apk`, both from Linux. Output goes to `dist/<target>`; `-arch` picks `amd64` or `arm64`, and `-console` keeps a Windows console. Android needs a JDK and an SDK with the NDK in `ANDROID_HOME` or `~/Android`, and packages Go apps only. A plain `func main()` works unchanged. The safe area is applied for you, so `Width` and `Height` exclude system bars and the keyboard.

On Linux, `SetAppID` must equal the app's `.desktop` basename or the compositor shows the wrong icon and name. `sys.Install` writes that file.

## Linux install, services and capture

`github.com/torisetxd/qui/sys` does not link the UI, so daemons can use it. It needs `gio-2.0 gio-unix-2.0 libpipewire-0.3`. C++ uses `sys/qui_sys.h` and `sys/qui_sys.hpp`, and links the `qui_sys` target.

```go
package main

import (
	"errors"
	"log"
	"time"

	"github.com/torisetxd/qui/sys"
)

func main() {
	const id = "dev.example.recorder"
	app := sys.App{ID: id, Name: "Recorder", Service: &sys.Service{Args: "--background", NeedsPipeWire: true}}
	if err := sys.Install(app); err != nil {
		log.Fatal(err)
	}
	screen, err := sys.OpenScreen(sys.ScreenOptions{AppID: id, FPS: 30, ShowCursor: true})
	if err != nil {
		log.Fatal(err)
	}
	defer screen.Close()
	for {
		frame, err := screen.Next(0, time.Second)
		if errors.Is(err, sys.ErrTimeout) {
			continue
		}
		if err != nil {
			return
		}
		log.Println(frame.Width, frame.Height, frame.Sequence)
	}
}
```

- **Use one id everywhere**: `qui.SetAppID`, `App.ID` and `ScreenOptions.AppID`. It becomes the `.desktop` name, portal identity, unit `app-<id>.service` and `$XDG_STATE_HOME/<id>`.
- **`Install` before the first `OpenScreen`.** It is safe on every launch and leaves a service the user disabled alone. `Exec` defaults to the running binary and must be absolute; `InstallAs` copies the binary to `~/.local/bin`.
- `OpenScreen` shows the portal dialog once and saves the permission; later runs restore it. Use `Slot` to hold several. `Multiple` and `Windows` widen the picker, `NoPersist` keeps nothing.
- `Next(stream, timeout)` returns the newest BGRx frame, valid until the next `Next` on that stream. Use one goroutine per stream. Retry on `ErrTimeout`; `ErrClosed` means sharing stopped.
- `sys.OpenAudio(sys.AudioOptions{})` captures the microphone, `System: true` the default output, `Target` a PipeWire node. `Read(buf, timeout)` fills interleaved `float32` and returns frames and the monotonic time of the first.
- Also: `Uninstall StartService StopService RestartService EnableService DisableService ServiceActive Register`.

## Verify without a window

Any of the first three variables runs the app offscreen for a few frames and exits. Inspect the PNG or JSON rather than assuming the UI is right. Offscreen runs use the CPU backend, which skips shaders, unless `QUI_BACKEND=gl` is set. Frames run faster than real time, so raise `QUI_FRAMES` if an animation has not settled.

| Variable | Effect |
| --- | --- |
| `QUI_SHOT=a.png` | Write a screenshot |
| `QUI_TREE=a.json` | Dump every node: `id parent kind x y w h text placeholder hit focused caret anchor` |
| `QUI_WIRE=a.bin` | Dump the frame's wire bytes |
| `QUI_SIZE=1100x700` | Viewport size |
| `QUI_DPR=2` | Pixel ratio |
| `QUI_BACKEND=cpu` or `gl` | Force a renderer |
| `QUI_FRAMES=n` | Minimum frames before exiting |
| `QUI_DRIVE='click:58,231;type:hello;key:ctrl+a'` | Scripted input, `;` separated, in logical pixels: `click:x,y`, `dblclick:x,y`, `rclick:x,y`, `drag:x0,y0,x1,y1`, `down:x,y`, `up:x,y`, `move:x,y`, `wheel:x,y,amount` (negative scrolls down), `type:text`, `key:shift+left`, `ime:text`, `wait:frames`, `dragover:x,y`, `drop:x,y,path`. Touch: `tap:x,y`, `longpress:x,y`, `swipe:x0,y0,x1,y1,ms`, `pinch:x,y,spread0,spread1`, `touch:x,y`, `touchmove:x,y`, `touchup:x,y` |

## Not available yet

Do not assume these exist: animated images, right-to-left paragraphs (mixed text orders correctly, but the base direction is always left-to-right), global shortcuts. On the web, list spacers use the estimate, so the scrollbar can shift when real heights differ.

To add a node kind, op or token, edit `tools/gen/spec.txt` and run `go run ./tools/gen`; `docs/wire.md` documents the format.
