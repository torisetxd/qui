//go:build cgo

package qui

import native "github.com/torisetxd/qui/src"

type platform struct {
	app *native.App
}

// SetAppID sets the Wayland app id. Call it before Open. It must match the
// basename of the app's .desktop file.
func SetAppID(id string) { native.SetAppID(id) }

// SetBorderless opens the window without the system title bar and border.
// Call it before Open. Mark the title bar with Style.Drag and draw your own
// window buttons; the edges still resize it.
func SetBorderless(on bool) { native.SetBorderless(on) }

// SetTransparent gives the window an alpha channel and starts each frame fully
// transparent. Call it before Open. A root box with a Bg and Radius then gives
// the window rounded corners; square them off while WindowMaximized.
func SetTransparent(on bool) { native.SetTransparent(on) }

// SetAlwaysOnTop keeps the window above other windows, where the compositor
// allows it; Wayland has no protocol for it, so there a window rule is still
// needed. Call it before Open.
func SetAlwaysOnTop(on bool) { native.SetAlwaysOnTop(on) }

// SetUtilityWindow leaves the window out of the task bar and the window list,
// for an overlay or a palette. Call it before Open.
func SetUtilityWindow(on bool) { native.SetUtilityWindow(on) }

// SetFocusable(false) stops the window from taking the keyboard focus, for an
// overlay that only shows something. Call it before Open.
func SetFocusable(on bool) { native.SetFocusable(on) }

// SetStartHidden opens with no window, until ShowWindow. Call it before Open.
func SetStartHidden(on bool) { native.SetStartHidden(on) }

// ShowWindow shows or hides the window while the loop keeps running.
func (u *Ui) ShowWindow(on bool) { u.app.ShowWindow(on) }

// HideWindow hides the window while the loop keeps running.
func (u *Ui) HideWindow() { u.app.ShowWindow(false) }

func (u *Ui) MinimizeWindow()  { u.app.WindowAction(0) }
func (u *Ui) ToggleMaximized() { u.app.WindowAction(1) }

// CloseWindow closes the window as its close button would, so Frame returns
// false.
func (u *Ui) CloseWindow() { u.app.WindowAction(2) }

func (u *Ui) WindowMaximized() bool { return u.app.WindowMaximized() }
func (u *Ui) WindowVisible() bool   { return u.app.WindowVisible() }
func (u *Ui) WindowMinimized() bool { return u.app.WindowMinimized() }

type CloseAction int

const (
	CloseQuit CloseAction = iota
	CloseHide
	CloseMinimize
)

// SetCloseAction controls what closing the window does: CloseQuit exits the
// loop (default), CloseHide hides the window, and CloseMinimize minimizes it
// to the taskbar.
func (u *Ui) SetCloseAction(action CloseAction) { u.app.SetCloseAction(int(action)) }

// Quit exits the frame loop even when SetCloseAction is not CloseQuit.
func (u *Ui) Quit() { u.app.Quit() }

type Tray struct{ t *native.Tray }
type TrayMenu struct{ m *native.TrayMenu }
type TrayEntry struct{ e *native.TrayEntry }

func (u *Ui) NewTray(tooltip string) *Tray { return &Tray{t: u.app.NewTray(tooltip)} }
func (t *Tray) Close()                     { t.t.Close() }
func (t *Tray) Valid() bool                { return t != nil && t.t.Valid() }
func (t *Tray) SetIcon(w, h int, rgba []byte) {
	t.t.SetIcon(w, h, rgba)
}
func (t *Tray) SetTooltip(tooltip string) { t.t.SetTooltip(tooltip) }
func (t *Tray) Menu() *TrayMenu {
	m := t.t.Menu()
	if m == nil {
		return nil
	}
	return &TrayMenu{m: m}
}
func (t *Tray) NextClick() (uint32, bool) { return t.t.NextClick() }

func (m *TrayMenu) AddItem(label string, id uint32) *TrayEntry {
	return m.InsertItem(-1, label, id)
}
func (m *TrayMenu) InsertItem(pos int, label string, id uint32) *TrayEntry {
	if m == nil {
		return nil
	}
	if e := m.m.AddItem(pos, label, id); e != nil {
		return &TrayEntry{e: e}
	}
	return nil
}
func (m *TrayMenu) AddCheck(label string, checked bool, id uint32) *TrayEntry {
	return m.InsertCheck(-1, label, checked, id)
}
func (m *TrayMenu) InsertCheck(pos int, label string, checked bool, id uint32) *TrayEntry {
	if m == nil {
		return nil
	}
	if e := m.m.AddCheck(pos, label, checked, id); e != nil {
		return &TrayEntry{e: e}
	}
	return nil
}
func (m *TrayMenu) AddSubmenu(label string) (*TrayEntry, *TrayMenu) {
	return m.InsertSubmenu(-1, label)
}
func (m *TrayMenu) InsertSubmenu(pos int, label string) (*TrayEntry, *TrayMenu) {
	if m == nil {
		return nil, nil
	}
	e, sub := m.m.AddSubmenu(pos, label)
	var te *TrayEntry
	var tm *TrayMenu
	if e != nil {
		te = &TrayEntry{e: e}
	}
	if sub != nil {
		tm = &TrayMenu{m: sub}
	}
	return te, tm
}
func (m *TrayMenu) AddSeparator() *TrayEntry { return m.InsertSeparator(-1) }
func (m *TrayMenu) InsertSeparator(pos int) *TrayEntry {
	if m == nil {
		return nil
	}
	if e := m.m.AddSeparator(pos); e != nil {
		return &TrayEntry{e: e}
	}
	return nil
}
func (e *TrayEntry) SetLabel(label string) {
	if e != nil {
		e.e.SetLabel(label)
	}
}
func (e *TrayEntry) SetChecked(checked bool) {
	if e != nil {
		e.e.SetChecked(checked)
	}
}
func (e *TrayEntry) Checked() bool { return e != nil && e.e.Checked() }
func (e *TrayEntry) SetEnabled(enabled bool) {
	if e != nil {
		e.e.SetEnabled(enabled)
	}
}
func (e *TrayEntry) Remove() {
	if e != nil {
		e.e.Remove()
	}
}

// Open creates the app window. Call it from the main goroutine.
func Open(title string, width, height int) *Ui {
	u := newUi()
	u.app = native.Open(title, width, height)
	return u
}

func (u *Ui) Close() { u.app.Close() }

// Wake requests a frame. It is safe to call from any goroutine.
func (u *Ui) Wake() { u.app.Wake() }

func (u *Ui) consumeBack() { u.app.ConsumeBack() }

// PickFiles opens the system file picker without blocking. The chosen paths
// arrive later through PickedFiles, and the app is woken when they do. Native
// only.
func (u *Ui) PickFiles(multiple bool) { u.app.PickFiles(multiple) }

// PickedFiles returns and clears the paths chosen in the file picker.
func (u *Ui) PickedFiles() []string {
	var out []string
	for {
		path, ok := u.app.PickedFile()
		if !ok {
			return out
		}
		out = append(out, path)
	}
}

// SetClipboard puts text on the system clipboard.
func (u *Ui) SetClipboard(text string) { u.app.SetClipboard(text) }

// FilesHovering reports whether files are dragged over the window. Dropped
// files arrive through PickedFiles.
func (u *Ui) FilesHovering() bool { return u.app.FilesHovering() }

// SelectedText returns the text selected in a Select box as of the last frame.
func (u *Ui) SelectedText() string { return u.app.SelectedText() }

// SetSmoothScroll eases mouse wheel steps toward their target. Off by default.
func (u *Ui) SetSmoothScroll(on bool) { u.app.SetSmoothScroll(on) }

func (u *Ui) submit() { u.app.Submit(u.buf) }

func (u *Ui) wait() []byte { return u.app.Wait() }
