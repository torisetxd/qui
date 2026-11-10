# Wire format

An app describes each frame as one flat byte stream. The native engine and the
browser DOM patcher both consume this stream, so a language only needs an
encoder to get every backend. Constants live in `tools/gen/spec.txt`.

All values are little-endian. `str` is a `u32` byte length followed by UTF-8.

## Frame stream

The stream starts with `Frame` and then holds nodes in pre-order. A node is
`Open`, its property ops, its children, then `End`.

| Op | Payload |
| --- | --- |
| `Frame` | `version:u8 theme:u8` |
| `ImageData` | `key:u64 w:u16 h:u16` then `w*h*4` bytes of straight RGBA. Sent once per key, directly after `Palette`, and the engine keeps it |
| `ImageShared` | `key:u64 token:u64`, directly after `Palette` like `ImageData`. Same as `ImageData`, but the pixels were lent to the engine out of band under `token` (`qui_stage_image`) and are shared rather than carried in the stream |
| `Img` | `key:u64` the image an `Image` node shows, or the SVG an `Icon` node shows |
| `SvgData` | `key:u64` then `str` of SVG text. Sent once per key, directly after `Palette` like `ImageData`, and the engine keeps it. `Icon` nodes draw it fitted to their box in their `Fg` (`Ink` when unset), using only the shapes' coverage |
| `Fit` | `u8`: `Cover` 0, `Contain` 1, `Stretch` 2 |
| `Span` | `start:u32 end:u32 flags:u8 fg:u32 bg:u32 id:u64 image:u64`, after `Text`. Styles a byte range; `id` non-zero makes it a hit target reported under that id. `flags`: `Bold` 1, `Mono` 2, `Italic` 4, `Underline` 8, `Strike` 16, `HoverUnderline` 64 (underlines only while the span is hovered or pressed), `Image` 32 (draws the loaded image `image` over the span's placeholder text) |
| `Jump` | `u32` on a `List`: scrolls that item to the top on this frame only |
| `Glide` | none, with `Jump`: eases the item to the middle of the viewport over the next frames instead, cutting to a screen away first when it is far; user scrolling cancels it |
| `ImageFree` | `key:u64`, directly after `Palette` like `ImageData`: forgets the image and its atlas space |
| `Prepend` | `u32` on a `List`: items inserted at the front this frame; the engine shifts its scroll anchor and cached heights by that count |
| `Palette` | `count:u16` then `count` of `token:u8 rgba:u32`. Optional, directly after `Frame`. Overrides or adds theme tokens for this frame; ids 20 to 255 are free for apps and show up on the web as `--qui-T<id>` |
| `Open` | `kind:u8 id:u64` |
| `End` | none |
| `Dir` | `u8` |
| `Width`, `Height` | `mode:u8 value:f32` |
| `MinW`, `MaxW`, `MinH`, `MaxH`, `Gap`, `Radius`, `Opacity`, `FontSize`, `Fps` | `f32` |
| `Value` | `f32`, the state of a `Switch` (0 or 1) or `Slider` (0 to 1) box, for assistive tech |
| `Role` `Button` | on a `Hit` box: takes focus, clicks on Enter and Space, gets the hover and press tints like a `Button` node, and is role=button on the web |
| `Pad` | `top:f32 right:f32 bottom:f32 left:f32` |
| `Align`, `Justify`, `Hit`, `Role`, `Weight` | `u8` |
| `Gestures` | `u8` mask of `PanX`, `PanY`, `Pinch` and `TouchOnly` |
| `Multiline` | `u8`, 1 for Enter inserting a newline, 2 for Enter submitting |
| `Focus` | `u8`, 1 to focus on first appearance, 2 to focus now |
| `Pos` | `x:f32 y:f32` |
| `Z` | `i32`, paint and hit order among siblings; ties keep tree order |
| `Gradient` | `from:u32 to:u32 angle:f32`, CSS degrees; fills a box, or colors the glyphs of a `Text` |
| `Shadow` | `color:u32 blur:f32 y:f32`, painted outside the box only |
| `Tracking` | `f32`, extra advance per letter in logical px; turns ligatures off when nonzero |
| `ScrollTo` | `f32`, sets the scroll offset of a `Scroll` box this frame |
| `Bg`, `Fg` | `color:u32` |
| `Border` | `color:u32 width:f32` |
| `Clip`, `Scroll`, `Mono`, `Ellipsis`, `Password`, `PassthroughNav`, `Select`, `Wrap` | none |
| `Select` | none: starts a selection root, so a mouse selection that begins inside stays inside. The frame root is one, so all text is selectable unless turned off |
| `NoSelect` | none: no text selection in this subtree until a `Select` below. Boxes with role `Button`, `Menu`, `MenuItem`, `Switch` or `Slider` behave as if they had it |
| `Ease` | `f32` milliseconds: changes to `Bg`, `Fg`, `Border`, `Radius`, `Opacity` and the button hover tint animate (ease-out cubic) instead of jumping. State is kept per node id; a node absent for a frame starts fresh. On the web it is a CSS `transition` |
| `HoverBg` | `color:u32`: the background while the pointer is over this box or any hit inside it, in place of a button's hover tint. On the web it is `:hover` |
| `Drag` | none: a press on this box, outside the hit boxes inside it, moves the window. Native desktop only; it is for windows opened borderless (`qui_set_borderless`) |
| `Fixed` | none: with `Pos`, the box is placed in window coordinates instead of its parent's; `Fill` takes the window's size, and the parent's clip and scroll do not apply |
| `NoScrollbar` | none: a `Scroll` box or `Virtual` list scrolls without drawing a scrollbar or taking scrollbar drags. On the web it is `scrollbar-width: none` |
| `Bare` | none, on an `Input` directly after `Open`: clears its default background and border, so a focused field shows no accent border |
| `Label`, `Text`, `Src`, `Placeholder` | `str` |
| `Virtual` | `count:u32 estimate:f32 bottom:u8` |
| `Item` | `index:u32` |
| `Uniform` | `name_len:u8 name n:u8 f32 * n` |

A color is `0xRRGGBBAA`. A theme token is `token << 24` with the low 24 bits
clear, which would otherwise be an invisible color.

## Results table

The backend answers each frame with a table the app reads while building the
next one, so widget calls never cross into the engine.

The header is 64 bytes: `magic:u32 capacity:u32 width:f32 height:f32 dpr:f32
dt:f32 pointer_x:f32 pointer_y:f32 keys_offset:u32 keys_count:u32
text_offset:u32 pointer_down:u32 engine_ms:f32 window_focused:u32
pointer_touch:u32` and 4 reserved bytes. It is followed by `capacity` slots of 56 bytes,
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
