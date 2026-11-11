package qui

import (
	"encoding/binary"
	"math"
)

const hashSeed uint64 = 14695981039346656037

type Size struct {
	mode  uint8
	value float32
}

func Px(v float32) Size         { return Size{sizePx, v} }
func Fill() Size                { return Size{sizeFill, 1} }
func FillN(weight float32) Size { return Size{sizeFill, weight} }
func Pct(percent float32) Size  { return Size{sizePct, percent} }

type Edges struct {
	Top, Right, Bottom, Left float32
}

func All(v float32) Edges   { return Edges{v, v, v, v} }
func XY(x, y float32) Edges { return Edges{y, x, y, x} }

func RGBA(r, g, b, a uint8) Color {
	return Color(uint32(r)<<24 | uint32(g)<<16 | uint32(b)<<8 | uint32(a))
}

func RGB(r, g, b uint8) Color { return RGBA(r, g, b, 255) }

// Style and TextStyle, with their encoders and class merges, are generated
// from tools/gen/spec.txt into style_gen.go.

// Gradient is a two-color linear gradient. Angle is in CSS degrees: 0 runs
// bottom to top, 90 left to right, 180 top to bottom. On text it runs across
// the text box and only follows the axis nearest to Angle.
type Gradient struct {
	From, To Color
	Angle    float32
}

// BoxShadow paints outside a box, like a CSS box-shadow. Use a bright Color
// and no Y for a glow.
type BoxShadow struct {
	Color Color
	Blur  float32
	Y     float32
}

var (
	Body  = TextStyle{}
	H1    = TextStyle{Size: 24, Bold: true, Color: Strong, Role: RoleHeading}
	H2    = TextStyle{Size: 18, Bold: true, Color: Strong, Role: RoleHeading}
	Small = TextStyle{Size: 12, Color: Muted}
	Code  = TextStyle{Size: 13, Mono: true}
)

type Response struct {
	Found, Hovered, Pressed, Clicked, RightClicked bool
	Focused, DragStart, Dragging, DragEnd          bool
	Changed, Submitted                             bool
	// LongPressed fires for a touch held in place, and also sets RightClicked.
	LongPressed, DoubleTapped           bool
	PanStart, Panning, PanEnd, Pinching bool
	X, Y, W, H, DX, DY                  float32
	// VX and VY are the pan velocity in pixels per second.
	VX, VY float32
	// Scale is this frame's pinch factor, 1 when not pinching.
	Scale float32
	// Swipe is the direction of a pan that ended at speed this frame.
	Swipe        Swipe
	First, Count uint32
	// Error holds the compile log of a failed Shader.
	Error string
}

// Stats describes the last submitted frame.
type Stats struct {
	Nodes, WireBytes int
	// EngineMs is the backend's cost for that frame, without waiting for vsync.
	EngineMs float32
}

type ListOptions struct {
	Style  Style
	Bottom bool
	// Prepend says that many items were inserted at the front of the list this
	// frame, so the rows on screen keep their place. Native only: the browser
	// ignores it.
	Prepend int
	// JumpTo scrolls that item to the top of the list on this frame only. Native
	// only: the browser ignores it.
	JumpTo *int
	// Glide makes JumpTo ease the item to the middle of the list instead.
	Glide bool
}

type Uniform struct {
	Name   string
	Values []float32
}

type ShaderOptions struct {
	Src      string
	Style    Style
	Fps      float32
	Uniforms []Uniform
}

type InputOptions struct {
	Style                                                     Style
	Placeholder                                               string
	Size                                                      float32
	Mono, Multiline, EnterSubmits, Password, Autofocus, Focus bool
	// PassthroughNav hands Up, Down and Tab to the app instead of the field.
	PassthroughNav bool
	// Bare drops the field's own background and border, focus accent
	// included, for a field drawn inside a box of your own.
	Bare bool
}

type scope struct {
	id    uint64
	index uint32
}

type Ui struct {
	platform
	buf     []byte
	stack   []scope
	res     []byte
	theme   Theme
	palette []paletteEntry

	imageAt     int
	pendingImgs []byte
	sentImages  map[uint64]struct{}
	sentSvgs    map[uint64]struct{}

	styleClasses map[string]Style
	textClasses  map[string]TextStyle
	started      bool
	reduceMotion bool
	sendKeys     bool
	nodes        int
	stats        Stats
	frame        uint64
	tweens       map[uint64]*tween
}

func newUi() *Ui {
	return &Ui{buf: make([]byte, 0, 1<<16), stack: make([]scope, 0, 64)}
}

// Frame submits the frame just built and blocks until the next one is needed.
// It returns false when the app should exit.
func (u *Ui) Frame() bool {
	if u.started {
		u.stats.Nodes, u.stats.WireBytes = u.nodes, len(u.buf)
		u.flushImages()
		u.submit()
	}
	res := u.wait()
	if res == nil {
		return false
	}
	u.res = res
	u.started = true
	u.sendKeys = len(res) >= resultsHeader && binary.LittleEndian.Uint32(res[60:])&1 != 0
	u.buf = u.buf[:0]
	u.nodes = 0
	u.frame++
	for id, t := range u.tweens {
		if t.seen+1 < u.frame {
			delete(u.tweens, id)
		}
	}
	u.stack = append(u.stack[:0], scope{hashSeed, 0})
	u.u8(opFrame)
	u.u8(wireVersion)
	u.u8(uint8(u.theme))
	u.emitPalette()
	u.imageAt = len(u.buf)
	return true
}

func (u *Ui) SetTheme(t Theme)  { u.theme = t }
func (u *Ui) Width() float32    { return u.header(8) }
func (u *Ui) Height() float32   { return u.header(12) }
func (u *Ui) Dpr() float32      { return u.header(16) }
func (u *Ui) Dt() float32       { return u.header(20) }
func (u *Ui) PointerX() float32 { return u.header(24) }
func (u *Ui) PointerY() float32 { return u.header(28) }

// SetReduceMotion makes drawers, Ease and Tween snap instead of animating.
func (u *Ui) SetReduceMotion(on bool) { u.reduceMotion = on }

// PointerDown reports whether the primary pointer button is held anywhere, or
// was pressed since the last frame, so a tap shorter than a frame still counts.
func (u *Ui) PointerDown() bool {
	return len(u.res) >= resultsHeader && binary.LittleEndian.Uint32(u.res[44:]) != 0
}

// PointerTouch reports whether the pointer is a finger. A lifted finger leaves
// PointerX and PointerY where it was, so skip hover effects while it is up.
func (u *Ui) PointerTouch() bool {
	return len(u.res) >= resultsHeader && binary.LittleEndian.Uint32(u.res[56:]) != 0
}

// Stats describes the last submitted frame.
func (u *Ui) Stats() Stats {
	stats := u.stats
	stats.EngineMs = u.header(48)
	return stats
}

func (u *Ui) Column(s Style, body func()) Response { return u.container(dirColumn, &s, body) }
func (u *Ui) Row(s Style, body func()) Response    { return u.container(dirRow, &s, body) }
func (u *Ui) Stack(s Style, body func()) Response  { return u.container(dirStack, &s, body) }

func (u *Ui) Text(value string, style ...TextStyle) {
	var t TextStyle
	if len(style) > 0 {
		t = style[0]
	}
	u.text(value, nil, t)
}

func (u *Ui) text(value string, spans []Span, t TextStyle) (links []linkSpan) {
	t = u.resolveText(t)
	id := u.open(kindText, "")
	u.str(opText, value)
	links = u.emitSpans(id, value, spans)
	u.encodeTextStyle(&t)
	u.u8(opEnd)
	return links
}

func (u *Ui) Button(label string, style ...Style) Response {
	var s Style
	if len(style) > 0 {
		s = style[0]
	}
	key := s.Key
	if key == "" {
		key = label
	}
	id := u.open(kindButton, key)
	u.emitStyle(&s)
	u.str(opText, label)
	u.u8(opEnd)
	return u.lookup(id)
}

func (u *Ui) List(key string, count int, estimate float32, o ListOptions, row func(i int)) Response {
	id := u.open(kindList, key)
	u.emitStyle(&o.Style)
	u.u8(opVirtual)
	u.u32(uint32(count))
	u.f32(estimate)
	if o.Bottom {
		u.u8(1)
	} else {
		u.u8(0)
	}
	if o.JumpTo != nil {
		if o.Glide && !u.reduceMotion {
			u.u8(opGlide)
		}
		u.u8(opJump)
		u.u32(uint32(*o.JumpTo))
	}
	if o.Prepend > 0 {
		u.u8(opPrepend)
		u.u32(uint32(o.Prepend))
	}
	result := u.lookup(id)
	first, visible := int(result.First)+o.Prepend, int(result.Count)
	if !result.Found || visible == 0 {
		visible = min(count, 32)
		first = 0
		if o.Bottom && count > visible {
			first = count - visible
		}
	}
	u.stack = append(u.stack, scope{id, 0})
	for i := first; i < min(first+visible, count); i++ {
		item := mixU32(id, uint32(i))
		u.nodes++
		u.u8(opOpen)
		u.u8(kindBox)
		u.u64(item)
		u.sizeOp(opWidth, Fill())
		u.u8(opItem)
		u.u32(uint32(i))
		u.stack = append(u.stack, scope{item, 0})
		row(i)
		u.stack = u.stack[:len(u.stack)-1]
		u.u8(opEnd)
	}
	u.stack = u.stack[:len(u.stack)-1]
	u.u8(opEnd)
	return result
}

// Input draws an editable text field bound to value. value is updated when
// Response.Changed is true.
func (u *Ui) Input(key string, value *string, options ...InputOptions) Response {
	var o InputOptions
	if len(options) > 0 {
		o = options[0]
	}
	id := u.open(kindInput, key)
	result := u.lookup(id)
	if result.Changed {
		start := binary.LittleEndian.Uint32(u.res[40:]) + result.First
		*value = string(u.res[start : start+result.Count])
	}
	result.First, result.Count = 0, 0
	if o.Bare {
		u.u8(opBare)
	}
	u.emitStyle(&o.Style)
	u.str(opText, *value)
	if o.Placeholder != "" {
		u.str(opPlaceholder, o.Placeholder)
	}
	if o.Size != 0 {
		u.f32Op(opFontSize, o.Size)
	}
	if o.Mono {
		u.u8(opMono)
	}
	if o.Multiline {
		if o.EnterSubmits {
			u.u8Op(opMultiline, 2)
		} else {
			u.u8Op(opMultiline, 1)
		}
	}
	if o.Password {
		u.u8(opPassword)
	}
	if o.PassthroughNav {
		u.u8(opPassthroughNav)
	}
	if o.Focus {
		u.u8Op(opFocus, 2)
	} else if o.Autofocus {
		u.u8Op(opFocus, 1)
	}
	u.u8(opEnd)
	return result
}

// KeyPressed reports whether key was pressed since the last frame with exactly
// the given modifiers held. Letters are their lowercase rune, as in
// KeyPressed('k', ModCtrl).
func (u *Ui) KeyPressed(key Key, mods ...Mods) bool {
	if len(u.res) < resultsHeader {
		return false
	}
	var want Mods
	for _, m := range mods {
		want |= m
	}
	at := binary.LittleEndian.Uint32(u.res[32:])
	count := binary.LittleEndian.Uint32(u.res[36:])
	for i := uint32(0); i < count; i++ {
		event := u.res[at+i*8:]
		if Key(binary.LittleEndian.Uint32(event)) == key && Mods(binary.LittleEndian.Uint32(event[4:])) == want {
			if key == KeyEscape {
				u.consumeBack()
			}
			return true
		}
	}
	return false
}

// Shader draws a GLSL fragment shader. A shader that fails to compile paints
// its background and reports the compiler log in Response.Error.
func (u *Ui) Shader(o ShaderOptions) Response {
	id := u.open(kindShader, o.Style.Key)
	result := u.lookup(id)
	result.First, result.Count = 0, 0
	u.emitStyle(&o.Style)
	u.str(opSrc, o.Src)
	if o.Fps != 0 {
		u.f32Op(opFps, o.Fps)
	}
	for i := range o.Uniforms {
		uniform := &o.Uniforms[i]
		u.u8(opUniform)
		u.u8(uint8(len(uniform.Name)))
		u.buf = append(u.buf, uniform.Name...)
		u.u8(uint8(len(uniform.Values)))
		for _, v := range uniform.Values {
			u.f32(v)
		}
	}
	u.u8(opEnd)
	return result
}

func mixString(h uint64, s string) uint64 {
	for i := 0; i < len(s); i++ {
		h ^= uint64(s[i])
		h *= 1099511628211
	}
	if h == 0 {
		return 1
	}
	return h
}

func mixU32(h uint64, v uint32) uint64 {
	for shift := 0; shift < 32; shift += 8 {
		h ^= uint64(byte(v >> shift))
		h *= 1099511628211
	}
	if h == 0 {
		return 1
	}
	return h
}

func (u *Ui) header(offset int) float32 {
	if len(u.res) < offset+4 {
		return 0
	}
	return math.Float32frombits(binary.LittleEndian.Uint32(u.res[offset:]))
}

func (u *Ui) u8(v uint8)   { u.buf = append(u.buf, v) }
func (u *Ui) u32(v uint32) { u.buf = binary.LittleEndian.AppendUint32(u.buf, v) }
func (u *Ui) u64(v uint64) { u.buf = binary.LittleEndian.AppendUint64(u.buf, v) }
func (u *Ui) f32(v float32) {
	u.buf = binary.LittleEndian.AppendUint32(u.buf, math.Float32bits(v))
}
func (u *Ui) u8Op(op, v uint8) {
	u.u8(op)
	u.u8(v)
}
func (u *Ui) u32Op(op uint8, v uint32) {
	u.u8(op)
	u.u32(v)
}
func (u *Ui) f32Op(op uint8, v float32) {
	u.u8(op)
	u.f32(v)
}
func (u *Ui) str(op uint8, v string) {
	u.u8(op)
	u.u32(uint32(len(v)))
	u.buf = append(u.buf, v...)
}
func (u *Ui) sizeOp(op uint8, s Size) {
	if s.mode == sizeFit {
		return
	}
	u.u8(op)
	u.u8(s.mode)
	u.f32(s.value)
}

func (u *Ui) open(kind uint8, key string) uint64 {
	top := &u.stack[len(u.stack)-1]
	var id uint64
	if key == "" {
		id = mixU32(top.id, top.index)
	} else {
		id = mixString(top.id, key)
	}
	top.index++
	u.nodes++
	u.u8(opOpen)
	u.u8(kind)
	u.u64(id)
	if u.sendKeys && key != "" {
		u.str(opKey, key)
	}
	return id
}

func (u *Ui) emitStyle(s *Style) {
	if s.Class != "" {
		resolved := u.resolveStyle(*s)
		s = &resolved
	}
	u.encodeStyle(s)
}

func (u *Ui) container(dir uint8, s *Style, body func()) Response {
	id := u.open(kindBox, s.Key)
	if dir != dirColumn {
		u.u8Op(opDir, dir)
	}
	u.emitStyle(s)
	u.stack = append(u.stack, scope{id, 0})
	body()
	u.stack = u.stack[:len(u.stack)-1]
	u.u8(opEnd)
	return u.lookup(id)
}

const (
	resultsHeader = 64
	resultsSlot   = 56
	swipeSpeed    = 300
)

func swipeOf(r *Response) Swipe {
	if !r.PanEnd {
		return SwipeNone
	}
	abs := func(v float32) float32 { return max(v, -v) }
	switch {
	case abs(r.VX) >= abs(r.VY) && r.VX <= -swipeSpeed:
		return SwipeLeft
	case abs(r.VX) >= abs(r.VY) && r.VX >= swipeSpeed:
		return SwipeRight
	case abs(r.VY) > abs(r.VX) && r.VY <= -swipeSpeed:
		return SwipeUp
	case abs(r.VY) > abs(r.VX) && r.VY >= swipeSpeed:
		return SwipeDown
	}
	return SwipeNone
}

func (u *Ui) lookup(id uint64) Response {
	out := Response{Scale: 1}
	if len(u.res) < resultsHeader {
		return out
	}
	capacity := binary.LittleEndian.Uint32(u.res[4:])
	if capacity == 0 {
		return out
	}
	mask := capacity - 1
	for i := uint32(id) & mask; ; i = (i + 1) & mask {
		slot := u.res[resultsHeader+int(i)*resultsSlot:]
		key := binary.LittleEndian.Uint64(slot)
		if key == 0 {
			return out
		}
		if key != id {
			continue
		}
		flags := binary.LittleEndian.Uint32(slot[8:])
		float := func(offset int) float32 {
			return math.Float32frombits(binary.LittleEndian.Uint32(slot[offset:]))
		}
		out.Found = true
		out.First = binary.LittleEndian.Uint32(slot[12:])
		out.Count = binary.LittleEndian.Uint32(slot[16:])
		out.X, out.Y, out.W, out.H = float(24), float(28), float(32), float(36)
		out.DX, out.DY = float(40), float(44)
		out.VX, out.VY = float(48), float(52)
		out.LongPressed = flags&flagLongPressed != 0
		out.DoubleTapped = flags&flagDoubleTapped != 0
		out.PanStart = flags&flagPanStart != 0
		out.Panning = flags&flagPanning != 0
		out.PanEnd = flags&flagPanEnd != 0
		out.Pinching = flags&flagPinching != 0
		if out.Pinching {
			out.Scale = float(20)
		}
		out.Swipe = swipeOf(&out)
		out.Hovered = flags&flagHovered != 0
		out.Pressed = flags&flagPressed != 0
		out.Clicked = flags&flagClicked != 0
		out.RightClicked = flags&flagRightClicked != 0
		out.Focused = flags&flagFocused != 0
		out.DragStart = flags&flagDragStart != 0
		out.Dragging = flags&flagDragging != 0
		out.DragEnd = flags&flagDragEnd != 0
		out.Changed = flags&flagChanged != 0
		out.Submitted = flags&flagSubmitted != 0
		if flags&flagShaderError != 0 {
			start := binary.LittleEndian.Uint32(u.res[40:]) + out.First
			out.Error = string(u.res[start : start+out.Count])
		}
		return out
	}
}
