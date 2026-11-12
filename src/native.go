//go:build cgo

package native

/*
#cgo CXXFLAGS: -std=c++20
#cgo linux,!android pkg-config: sdl3 freetype2 harfbuzz fontconfig
#cgo linux,!android LDFLAGS: -lstdc++ -lm
#cgo windows pkg-config: --static sdl3 freetype2 harfbuzz
#cgo windows LDFLAGS: -static
#cgo android pkg-config: --static sdl3 freetype2 harfbuzz
#cgo android LDFLAGS: -static-libstdc++ -llog -landroid -Wl,-z,max-page-size=16384
#include <stdlib.h>
#include "qui_c.h"
*/
import "C"

import "unsafe"

type App struct {
	app *C.qui_app
	n   C.size_t
}

func SetAppID(id string) {
	cid := C.CString(id)
	defer C.free(unsafe.Pointer(cid))
	C.qui_set_app_id(cid)
}

func SetBorderless(on bool) {
	v := C.int(0)
	if on {
		v = 1
	}
	C.qui_set_borderless(v)
}

func SetTransparent(on bool) {
	v := C.int(0)
	if on {
		v = 1
	}
	C.qui_set_transparent(v)
}

func Open(title string, width, height int) *App {
	ctitle := C.CString(title)
	defer C.free(unsafe.Pointer(ctitle))
	return &App{app: C.qui_open(ctitle, C.int(width), C.int(height))}
}

func (a *App) Close() { C.qui_close(a.app) }

func (a *App) Wake() { C.qui_wake(a.app) }

func (a *App) ConsumeBack() { C.qui_consume_back(a.app) }

func (a *App) PickFiles(multiple bool) {
	m := C.int(0)
	if multiple {
		m = 1
	}
	C.qui_pick_files(a.app, m)
}

func (a *App) PickedFile() (string, bool) {
	p := C.qui_picked_file(a.app)
	if p == nil {
		return "", false
	}
	return C.GoString(p), true
}

func (a *App) SetClipboard(text string) {
	ctext := C.CString(text)
	defer C.free(unsafe.Pointer(ctext))
	C.qui_set_clipboard(a.app, ctext)
}

func (a *App) FilesHovering() bool { return C.qui_files_hovering(a.app) != 0 }

func (a *App) WindowAction(action int) { C.qui_window_action(a.app, C.int(action)) }

func (a *App) WindowMaximized() bool { return C.qui_window_maximized(a.app) != 0 }

func (a *App) SelectedText() string { return C.GoString(C.qui_selected_text(a.app)) }

func (a *App) SetSmoothScroll(on bool) {
	v := C.int(0)
	if on {
		v = 1
	}
	C.qui_set_smooth_scroll(a.app, v)
}

func (a *App) Submit(buf []byte) {
	C.qui_frame_end(a.app, (*C.uint8_t)(unsafe.Pointer(unsafe.SliceData(buf))), C.size_t(len(buf)))
}

func (a *App) Wait() []byte {
	p := C.qui_frame_begin(a.app, &a.n)
	if p == nil {
		return nil
	}
	return unsafe.Slice((*byte)(unsafe.Pointer(p)), int(a.n))
}
