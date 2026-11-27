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

func cb(on bool) C.int {
	if on {
		return 1
	}
	return 0
}

func SetBorderless(on bool) { C.qui_set_borderless(cb(on)) }

func SetTransparent(on bool) { C.qui_set_transparent(cb(on)) }

func SetAlwaysOnTop(on bool)   { C.qui_set_always_on_top(cb(on)) }
func SetUtilityWindow(on bool) { C.qui_set_utility_window(cb(on)) }
func SetFocusable(on bool)     { C.qui_set_focusable(cb(on)) }
func SetStartHidden(on bool)   { C.qui_set_start_hidden(cb(on)) }

func (a *App) ShowWindow(on bool) { C.qui_show_window(a.app, cb(on)) }

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
func (a *App) WindowVisible() bool   { return C.qui_window_visible(a.app) != 0 }
func (a *App) WindowMinimized() bool { return C.qui_window_minimized(a.app) != 0 }
func (a *App) SetCloseAction(action int) {
	C.qui_set_close_action(a.app, C.int(action))
}
func (a *App) Quit() { C.qui_quit(a.app) }

type Tray struct{ p *C.qui_tray }
type TrayMenu struct{ p *C.qui_tray_menu }
type TrayEntry struct{ p *C.qui_tray_entry }

func (a *App) NewTray(tooltip string) *Tray {
	ctip := C.CString(tooltip)
	defer C.free(unsafe.Pointer(ctip))
	return &Tray{p: C.qui_tray_create(a.app, ctip)}
}

func (t *Tray) Close() {
	if t.p != nil {
		C.qui_tray_destroy(t.p)
		t.p = nil
	}
}

func (t *Tray) Valid() bool { return t != nil && C.qui_tray_valid(t.p) != 0 }

func (t *Tray) SetIcon(w, h int, rgba []byte) {
	var ptr *C.uint8_t
	if len(rgba) > 0 {
		ptr = (*C.uint8_t)(unsafe.Pointer(unsafe.SliceData(rgba)))
	}
	C.qui_tray_set_icon(t.p, C.int(w), C.int(h), ptr)
}

func (t *Tray) SetTooltip(tooltip string) {
	ctip := C.CString(tooltip)
	defer C.free(unsafe.Pointer(ctip))
	C.qui_tray_set_tooltip(t.p, ctip)
}

func (t *Tray) Menu() *TrayMenu {
	p := C.qui_tray_menu_root(t.p)
	if p == nil {
		return nil
	}
	return &TrayMenu{p: p}
}

func (t *Tray) NextClick() (uint32, bool) {
	var id C.uint32_t
	if C.qui_tray_next_click(t.p, &id) != 0 {
		return uint32(id), true
	}
	return 0, false
}

func (m *TrayMenu) AddItem(pos int, label string, id uint32) *TrayEntry {
	if m == nil {
		return nil
	}
	clabel := C.CString(label)
	defer C.free(unsafe.Pointer(clabel))
	p := C.qui_tray_add_item(m.p, C.int(pos), clabel, C.uint32_t(id))
	if p == nil {
		return nil
	}
	return &TrayEntry{p: p}
}

func (m *TrayMenu) AddCheck(pos int, label string, checked bool, id uint32) *TrayEntry {
	if m == nil {
		return nil
	}
	clabel := C.CString(label)
	defer C.free(unsafe.Pointer(clabel))
	p := C.qui_tray_add_check(m.p, C.int(pos), clabel, cb(checked), C.uint32_t(id))
	if p == nil {
		return nil
	}
	return &TrayEntry{p: p}
}

func (m *TrayMenu) AddSubmenu(pos int, label string) (*TrayEntry, *TrayMenu) {
	if m == nil {
		return nil, nil
	}
	clabel := C.CString(label)
	defer C.free(unsafe.Pointer(clabel))
	var sub *C.qui_tray_menu
	p := C.qui_tray_add_submenu(m.p, C.int(pos), clabel, &sub)
	var entry *TrayEntry
	var menu *TrayMenu
	if p != nil {
		entry = &TrayEntry{p: p}
	}
	if sub != nil {
		menu = &TrayMenu{p: sub}
	}
	return entry, menu
}

func (m *TrayMenu) AddSeparator(pos int) *TrayEntry {
	if m == nil {
		return nil
	}
	p := C.qui_tray_add_separator(m.p, C.int(pos))
	if p == nil {
		return nil
	}
	return &TrayEntry{p: p}
}

func (e *TrayEntry) SetLabel(label string) {
	if e == nil {
		return
	}
	clabel := C.CString(label)
	defer C.free(unsafe.Pointer(clabel))
	C.qui_tray_entry_set_label(e.p, clabel)
}

func (e *TrayEntry) SetChecked(checked bool) {
	if e != nil {
		C.qui_tray_entry_set_checked(e.p, cb(checked))
	}
}

func (e *TrayEntry) Checked() bool {
	return e != nil && C.qui_tray_entry_checked(e.p) != 0
}

func (e *TrayEntry) SetEnabled(enabled bool) {
	if e != nil {
		C.qui_tray_entry_set_enabled(e.p, cb(enabled))
	}
}

func (e *TrayEntry) Remove() {
	if e != nil && e.p != nil {
		C.qui_tray_remove_entry(e.p)
		e.p = nil
	}
}

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
