# Wire format

An app describes each frame as one flat byte stream. The native engine and the
browser DOM patcher both consume this stream, so a language only needs an
encoder to get every backend. The op table below and every constant are
generated from `tools/gen/spec.txt`.

All values are little-endian. `str` is a `u32` byte length followed by UTF-8.

## Frame stream

The stream starts with `Frame` and then holds nodes in pre-order. A node is
`Open`, its property ops, its children, then `End`.

<!-- gen:ops -->
| Op | Payload | Meaning |
| --- | --- | --- |
| `End` 0 | none | Closes the open node. |
| `Open` 1 | `kind:u8` `id:u64` | Opens a node; its property ops, children and `End` follow. |
| `Frame` 2 | `version:u8` `theme:u8` | Starts the stream. |
| `Palette` 45 | `count:u16` `entries:token_rgba` | Optional, directly after `Frame`: `count` pairs of `token:u8 rgba:u32` that override or add theme tokens for this frame. Ids 20 to 255 are free for apps and show up on the web as `--qui-T<id>`. |
| `ImageData` 46 | `key:u64` `w:u16` `h:u16` `pixels:rgba` | `w*h*4` bytes of straight RGBA. Sent once per key, directly after `Palette`, and the engine keeps it. |
| `ImageShared` 66 | `key:u64` `token:u64` | Like `ImageData`, but the pixels were lent to the engine out of band under `token` (`qui_stage_image`) and are shared rather than carried in the stream. Native only; the browser ignores it. |
| `SvgData` 57 | `key:u64` `svg:str` | SVG text, sent once per key directly after `Palette` like `ImageData`, and the engine keeps it. `Icon` nodes draw it fitted to their box in their `Fg` (`Ink` when unset), using only the shapes' coverage. |
| `ImageFree` 52 | `key:u64` | Directly after `Palette` like `ImageData`: forgets the image and its atlas space. |
| `Dir` 3 | `dir:u8` | `Column` 0, `Row` 1 or `Stack` 2. |
| `Width` 4 | `w_mode:u8` `w_val:f32` | A size: `Fit` 0, `Fill` 1 (the value is the weight), `Px` 2, `Pct` 3. |
| `Height` 5 | `h_mode:u8` `h_val:f32` | Like `Width`. |
| `MinW` 6 | `min_w:f32` |  |
| `MaxW` 7 | `max_w:f32` |  |
| `MinH` 8 | `min_h:f32` |  |
| `MaxH` 9 | `max_h:f32` |  |
| `Pad` 10 | `pad:f32x4` | Top, right, bottom, left. |
| `Gap` 11 | `gap:f32` |  |
| `Align` 12 | `align:u8` | Cross axis: `Start Center End Stretch`. |
| `Justify` 13 | `justify:u8` | Main axis: `Start Center End Between`; on text, the line alignment. |
| `Pos` 14 | `pos_x:f32` `pos_y:f32` | Places the box at x, y in its parent, out of the flow. |
| `Fixed` 64 | none | With `Pos`, the box is placed in window coordinates instead of its parent's; `Fill` takes the window's size, and the parent's clip and scroll do not apply. |
| `Z` 39 | `z:i32` | Paint and hit order among siblings; ties keep tree order. |
| `Bg` 15 | `bg:u32` | A color is `0xRRGGBBAA`; a theme token is `token << 24` with the low 24 bits clear. |
| `Fg` 16 | `fg:u32` |  |
| `Border` 17 | `border:u32` `border_w:f32` |  |
| `Radius` 18 | `radius:f32` |  |
| `Opacity` 19 | `opacity:f32` |  |
| `Gradient` 40 | `gradient_from:u32` `gradient_to:u32` `gradient_angle:f32` | CSS degrees; fills a box, or colors the glyphs of a `Text`. |
| `Shadow` 41 | `shadow:u32` `shadow_blur:f32` `shadow_y:f32` | Painted outside the box only. |
| `Clip` 20 | none |  |
| `Scroll` 21 | none | Scrolls vertically; needs a bounded height. |
| `NoScrollbar` 65 | none | A `Scroll` box or `Virtual` list scrolls without drawing a scrollbar or taking scrollbar drags. On the web it is `scrollbar-width: none`. |
| `ScrollTo` 43 | `scroll_to:f32` | Sets the scroll offset of a `Scroll` box this frame. |
| `Hit` 22 | `hit:u8` | Reports pointer input for the box. |
| `Select` 54 | none | Starts a selection root, so a mouse selection that begins inside stays inside. The frame root is one, so all text is selectable unless turned off. |
| `NoSelect` 59 | none | No text selection in this subtree until a `Select` below. Boxes with role `Button`, `Menu`, `MenuItem`, `Switch` or `Slider` behave as if they had it, and it wins over `Select` on the same box. |
| `Ease` 60 | `ease:f32` | Milliseconds: changes to `Bg`, `Fg`, `Border`, `Radius`, `Opacity` and the button hover tint animate (ease-out cubic) instead of jumping. State is kept per node id; a node absent for a frame starts fresh. On the web it is a CSS `transition`. |
| `HoverBg` 61 | `hover_bg:u32` | The background while the pointer is over this box or any hit inside it, in place of a button's hover tint. On the web it is `:hover`. |
| `Drag` 63 | none | A press on this box, outside the hit boxes inside it, moves the window. Native desktop only; it is for windows opened borderless (`qui_set_borderless`). Native only; the browser ignores it. |
| `Wrap` 55 | none | On a `Row`: flows children onto new lines. |
| `Gestures` 44 | `gestures:u8` | Mask of `PanX`, `PanY`, `Pinch` and `TouchOnly`. |
| `Role` 23 | `role:u8` | On the web it picks the element. `Button` on a `Hit` box: takes focus, clicks on Enter and Space, gets the hover and press tints like a `Button` node, and is role=button on the web. |
| `Label` 24 | `label:str` | The accessible name. |
| `Key` 67 | `key_name:str` | The key the node was opened with. Sent only when bit 0 of the results header's `debug` field is set, as offscreen runs do, so `QUI_TREE` and `QUI_DRIVE` selectors can name nodes. Native only; the browser ignores it. |
| `Value` 56 | `value:f32` | The state of a `Switch` (0 or 1) or `Slider` (0 to 1) box, for assistive tech. The engine ignores it. |
| `Text` 25 | `text:str` |  |
| `Span` 49 | `start:u32` `end:u32` `flags:u8` `fg:u32` `bg:u32` `id:u64` `image:u64` | After `Text`. Styles a byte range; `id` non-zero makes it a hit target reported under that id. `flags`: `Bold` 1, `Mono` 2, `Italic` 4, `Underline` 8, `Strike` 16, `HoverUnderline` 64 (underlines only while the span is hovered or pressed), `Image` 32 (draws the loaded image `image` over the span's placeholder text). |
| `FontSize` 26 | `font_size:f32` |  |
| `Weight` 27 | `weight:u8` | 1 for bold. |
| `Mono` 28 | none |  |
| `Ellipsis` 29 | none |  |
| `Tracking` 42 | `tracking:f32` | Extra advance per letter in logical px; turns ligatures off when nonzero. |
| `Virtual` 30 | `count:u32` `est:f32` `bottom:bool` | Makes a `List` virtual: item count, estimated row height, and whether it starts at the bottom. |
| `Item` 31 | `item:u32` | The index of a list row. |
| `Prepend` 50 | `prepend:u32` | On a `List`: items inserted at the front this frame; the engine shifts its scroll anchor and cached heights by that count. Native only; the browser ignores it. |
| `Jump` 51 | `jump:u32` | On a `List`: scrolls that item to the top on this frame only. Native only; the browser ignores it. |
| `Glide` 62 | none | With `Jump`: eases the item to the middle of the viewport over the next frames instead, cutting to a screen away first when it is far; user scrolling cancels it. Native only; the browser ignores it. |
| `Src` 32 | `src:str` | Shader source. |
| `Uniform` 33 | `name:str8` `count:u8` `values:f32xN` |  |
| `Fps` 34 | `fps:f32` |  |
| `Placeholder` 35 | `placeholder:str` |  |
| `Multiline` 36 | `multiline:u8` | 1 for Enter inserting a newline, 2 for Enter submitting. |
| `Password` 37 | none |  |
| `PassthroughNav` 53 | none | Hands Up, Down and Tab to the app instead of the field. |
| `Bare` 58 | none | On an `Input` directly after `Open`: clears its default background and border, so a focused field shows no accent border. |
| `Focus` 38 | `focus_mode:u8` | 1 to focus on first appearance, 2 to focus now. |
| `Img` 47 | `image:u64` | The image an `Image` node shows, or the SVG an `Icon` node shows. |
| `Fit` 48 | `fit:u8` | `Cover` 0, `Contain` 1, `Stretch` 2, plus `Pixelated` 16 or `Bicubic` 32. |
<!-- /gen:ops -->

A color is `0xRRGGBBAA`. A theme token is `token << 24` with the low 24 bits
clear, which would otherwise be an invisible color.

## Results table

The backend answers each frame with a table the app reads while building the
next one, so widget calls never cross into the engine.

The header is 64 bytes: `magic:u32 capacity:u32 width:f32 height:f32 dpr:f32
dt:f32 pointer_x:f32 pointer_y:f32 keys_offset:u32 keys_count:u32
text_offset:u32 pointer_down:u32 engine_ms:f32 window_focused:u32
pointer_touch:u32 debug:u32`. Bit 0 of `debug` asks the app to send `Key` after
each `Open` that has a key; offscreen runs set it. The header is followed by `capacity` slots of 56 bytes,
open-addressed with linear probing from `id & (capacity - 1)`. An empty slot has
`id == 0`.

After the slots come `keys_count` key events of `key:u32 mods:u32`, then the
UTF-8 text of every edited `Input` and every failed `Shader`.

| Offset | Field |
| --- | --- |
| 0 | `id:u64` |
| 8 | `flags:u32` |
| 12 | `first:u32` |
| 16 | `count:u32` |
| 20 | `scale:f32` |
| 24 | `x:f32 y:f32 w:f32 h:f32` |
| 40 | `dx:f32 dy:f32` |
| 48 | `vx:f32 vy:f32` |

`first` and `count` are the visible item range of a `List`. On an `Input` with
`Changed` set they are the byte offset from `text_offset` and the length of its
new value, and on a `Shader` with `ShaderError` set the same for its compile log.
A `Bottom` list sets `Pinned` while it rests at its end, so a client building
more items than it had can build the new ones in the same frame.

## Gestures

Once a press moves past the slop (8 px for touch, 4 for a mouse), its dominant
axis picks one owner. The backend walks from the innermost node under the press
to the root and stops at the first that can take it: a node whose `Gestures` has
that axis, or for touch on the vertical axis, a scroller that can still move
that way. If none can, the press continues as a drag. Winning cancels the press,
so no click follows.

`dx dy` carry the movement since the last frame, also during `Pinching`, and `vx
vy` the velocity in logical px per second during `Panning` and on `PanEnd`.
`scale` is the factor since the last frame and is only set with `Pinching`. A
touch held for 500 ms without moving sets `LongPressed` and `RightClicked` and
cancels the click. `DoubleTapped` comes with the second `Clicked`.

## Text fields

An `Input` carries the app's current value in `Text` every frame. The backend
owns the caret, selection and composition, and reports edits through `Changed`.
A value that differs from the last one reported is an edit by the app, so it
replaces the field's contents and moves the caret to the end.

Keys a focused field acts on are not reported as key events: printable keys,
caret movement, Enter, and Ctrl with `A C X V Z Y`. Tab moves focus and is never
reported, and Enter or Space on a focused button clicks it.

## Layout

Layout is a strict subset of CSS flexbox so the DOM backend maps it directly.
Every box is `box-sizing: border-box` with `min-width: 0` and `min-height: 0`.

| Wire | CSS |
| --- | --- |
| `Column`, `Row` | `display: flex` with `flex-direction` |
| `Stack` | one-cell grid, every child in cell `1 / 1` |
| `Pos` | `position: absolute` with `left` and `top` |
| `Fixed` | `position: fixed` with `left` and `top` |
| `Fit` on a row's width | `flex: 0 1 auto` |
| `Fit` elsewhere | `flex: 0 0 auto` |
| `Fill` on the main axis | `flex: <weight> 1 0` |
| `Fill` on the cross axis | `align-self: stretch` |
| `Px` | fixed size with `flex: 0 0 auto` |
| `Pct` | percentage size with `flex: 0 0 auto` |
