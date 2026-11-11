package qui

import (
	"encoding/binary"
	"sort"
	"strings"
)

// Token returns the theme color slot id. Ids 1 to 19 are the built-in tokens
// (Background .. Shadow); ids 20 to 255 belong to your app. Give them values
// with SetPalette and use them wherever a Color is expected, so they follow
// theme changes like the built-in ones.
func Token(id uint8) Color { return Color(uint32(id) << 24) }

type paletteEntry struct {
	id    uint8
	value Color
}

// SetPalette selects a built-in theme and overrides or adds tokens on top of
// it. The result is a complete custom theme: call it again with another map to
// switch themes, and every box and text that uses those tokens follows.
func (u *Ui) SetPalette(base Theme, colors map[Color]Color) {
	u.theme = base
	u.palette = u.palette[:0]
	for token, value := range colors {
		u.palette = append(u.palette, paletteEntry{uint8(token >> 24), value})
	}
	sort.Slice(u.palette, func(i, j int) bool { return u.palette[i].id < u.palette[j].id })
}

func (u *Ui) emitPalette() {
	if len(u.palette) == 0 {
		return
	}
	u.u8(opPalette)
	u.buf = append(u.buf, byte(len(u.palette)), byte(len(u.palette)>>8))
	for _, e := range u.palette {
		u.u8(e.id)
		u.u32(uint32(e.value))
	}
}

// Class registers a named style. Set Style.Class to a class name, or several
// separated by spaces, and the class fills every field you left at zero. Later
// classes override earlier ones, and the element's own fields win over both.
// A class may itself name other classes.
func (u *Ui) Class(name string, s Style) {
	if u.styleClasses == nil {
		u.styleClasses = map[string]Style{}
	}
	u.styleClasses[name] = s
}

// TextClass is Class for TextStyle.
func (u *Ui) TextClass(name string, t TextStyle) {
	if u.textClasses == nil {
		u.textClasses = map[string]TextStyle{}
	}
	u.textClasses[name] = t
}

func (u *Ui) resolveStyle(s Style) Style {
	names := strings.Fields(s.Class)
	s.Class = ""
	for i := len(names) - 1; i >= 0; i-- {
		base, ok := u.styleClasses[names[i]]
		if !ok {
			continue
		}
		if base.Class != "" {
			base = u.resolveStyle(base)
		}
		s = s.fill(base)
	}
	return s
}

func (u *Ui) resolveText(t TextStyle) TextStyle {
	names := strings.Fields(t.Class)
	t.Class = ""
	for i := len(names) - 1; i >= 0; i-- {
		base, ok := u.textClasses[names[i]]
		if !ok {
			continue
		}
		if base.Class != "" {
			base = u.resolveText(base)
		}
		t = t.fill(base)
	}
	return t
}

// ImageFit chooses how an Image fills its box: FitCover crops to fill it,
// FitContain shows the whole image, FitStretch distorts to fit.
type ImageFit uint8

// LoadImage hands the engine straight (not premultiplied) RGBA pixels under a
// key of your choosing, once. Later frames refer to it by key with Image and
// send no pixels. Loading the same key again replaces the picture. Images are
// at most 65535 pixels on a side; send them near the size you show them,
// because the engine keeps its own copy.
func (u *Ui) LoadImage(key string, w, h int, rgba []byte) {
	if w <= 0 || h <= 0 || w > 65535 || h > 65535 || len(rgba) != w*h*4 {
		return
	}
	id := mixString(0, key)
	if u.sentImages == nil {
		u.sentImages = map[uint64]struct{}{}
	}
	u.sentImages[id] = struct{}{}
	u.pendingImgs = append(u.pendingImgs, opImageData)
	u.pendingImgs = binary.LittleEndian.AppendUint64(u.pendingImgs, id)
	u.pendingImgs = binary.LittleEndian.AppendUint16(u.pendingImgs, uint16(w))
	u.pendingImgs = binary.LittleEndian.AppendUint16(u.pendingImgs, uint16(h))
	u.pendingImgs = append(u.pendingImgs, rgba...)
}

// FreeImage forgets a loaded image and its pixels.
func (u *Ui) FreeImage(key string) {
	id := mixString(0, key)
	if _, ok := u.sentImages[id]; !ok {
		return
	}
	delete(u.sentImages, id)
	u.pendingImgs = append(u.pendingImgs, opImageFree)
	u.pendingImgs = binary.LittleEndian.AppendUint64(u.pendingImgs, id)
}

// HasImage reports whether LoadImage was called for the key.
func (u *Ui) HasImage(key string) bool {
	_, ok := u.sentImages[mixString(0, key)]
	return ok
}

// Image draws a loaded picture. With no W and H it takes the image's pixel size;
// with only a width it keeps the aspect ratio. Radius clips it, so W == H and
// Radius == W/2 gives a circle. A key that was never loaded draws nothing but
// still takes its space once loaded.
func (u *Ui) Image(key string, s Style, fit ...ImageFit) Response {
	id := u.open(kindImage, s.Key)
	u.u8(opImg)
	u.u64(mixString(0, key))
	if len(fit) > 0 && fit[0] != FitCover {
		u.u8Op(opFit, uint8(fit[0]))
	}
	u.emitStyle(&s)
	u.u8(opEnd)
	return u.lookup(id)
}

func (u *Ui) flushImages() {
	if len(u.pendingImgs) == 0 {
		return
	}
	tail := append([]byte(nil), u.buf[u.imageAt:]...)
	u.buf = append(append(u.buf[:u.imageAt], u.pendingImgs...), tail...)
	u.pendingImgs = u.pendingImgs[:0]
}

// SpanFlags style a Span. Combine SpanBold, SpanMono, SpanItalic, SpanUnderline
// and SpanStrike.
type SpanFlags uint8

// Span styles the bytes Start to End of a Rich text. Fg and Bg of zero keep the
// text color and paint no background. A non-empty Link makes the span
// clickable: Rich reports it by name in Hovered, Clicked or RightClicked.
// Spans must be sorted and must not overlap; text between them is plain.
type Span struct {
	Start, End int
	Flags      SpanFlags
	Fg, Bg     Color
	Link       string
	// Image is the key of a loaded image drawn in place of the span. The span's
	// bytes must be ImagePlaceholder, which reserves its width.
	Image string
}

// ImagePlaceholder is the text to give a Span with an Image: a fixed-width gap
// about as wide as a custom emoji.
const ImagePlaceholder = "\u2003\u2002"

type linkSpan struct {
	name string
	id   uint64
}

// RichResult names the links under the pointer, from the previous frame.
type RichResult struct {
	Hovered, Clicked, RightClicked string
}

// Rich is Text with styled spans: bold, monospace, colors, backgrounds,
// underline, strike-through and clickable links, wrapped together as one
// paragraph. Offsets are UTF-8 byte offsets into value.
func (u *Ui) Rich(value string, spans []Span, style ...TextStyle) RichResult {
	var t TextStyle
	if len(style) > 0 {
		t = style[0]
	}
	var out RichResult
	for _, link := range u.text(value, spans, t) {
		r := u.lookup(link.id)
		if r.Hovered {
			out.Hovered = link.name
		}
		if r.Clicked {
			out.Clicked = link.name
		}
		if r.RightClicked {
			out.RightClicked = link.name
		}
	}
	return out
}

func (u *Ui) emitSpans(node uint64, value string, spans []Span) []linkSpan {
	var links []linkSpan
	for _, s := range spans {
		if s.Start < 0 || s.End > len(value) || s.End <= s.Start {
			continue
		}
		var id uint64
		if s.Link != "" {
			id = mixString(node, "link:"+s.Link)
			links = append(links, linkSpan{s.Link, id})
		}
		flags := s.Flags
		var image uint64
		if s.Image != "" {
			flags |= SpanImage
			image = mixString(0, s.Image)
		}
		u.u8(opSpan)
		u.u32(uint32(s.Start))
		u.u32(uint32(s.End))
		u.u8(uint8(flags))
		u.u32(uint32(s.Fg))
		u.u32(uint32(s.Bg))
		u.u64(id)
		u.u64(image)
	}
	return links
}
