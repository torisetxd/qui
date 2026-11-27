//go:build js

package qui

import "syscall/js"

type platform struct {
	bridge  js.Value
	out     js.Value
	outCap  int
	results []byte
	wake    chan struct{}
	onWake  js.Func
}

// SetAppID has no effect in the browser.
func SetAppID(string) {}

// SetBorderless, SetTransparent and the window controls have no effect in the
// browser.
func SetBorderless(bool)            {}
func SetTransparent(bool)           {}
func SetAlwaysOnTop(bool)           {}
func SetUtilityWindow(bool)         {}
func SetFocusable(bool)             {}
func SetStartHidden(bool)           {}
func (u *Ui) ShowWindow(bool)       {}
func (u *Ui) HideWindow()           {}
func (u *Ui) MinimizeWindow()       {}
func (u *Ui) ToggleMaximized()      {}
func (u *Ui) CloseWindow()          {}
func (u *Ui) WindowMaximized() bool { return false }
func (u *Ui) WindowVisible() bool   { return true }
func (u *Ui) WindowMinimized() bool { return false }

type CloseAction int

const (
	CloseQuit CloseAction = iota
	CloseHide
	CloseMinimize
)

func (u *Ui) SetCloseAction(CloseAction) {}
func (u *Ui) Quit()                      {}

type Tray struct{}
type TrayMenu struct{}
type TrayEntry struct{}

func (u *Ui) NewTray(string) *Tray                        { return &Tray{} }
func (t *Tray) Close()                                    {}
func (t *Tray) Valid() bool                               { return false }
func (t *Tray) SetIcon(int, int, []byte)                  {}
func (t *Tray) SetTooltip(string)                         {}
func (t *Tray) Menu() *TrayMenu                           { return nil }
func (t *Tray) NextClick() (uint32, bool)                 { return 0, false }
func (m *TrayMenu) AddItem(string, uint32) *TrayEntry     { return nil }
func (m *TrayMenu) InsertItem(int, string, uint32) *TrayEntry { return nil }
func (m *TrayMenu) AddCheck(string, bool, uint32) *TrayEntry  { return nil }
func (m *TrayMenu) InsertCheck(int, string, bool, uint32) *TrayEntry {
	return nil
}
func (m *TrayMenu) AddSubmenu(string) (*TrayEntry, *TrayMenu)      { return nil, nil }
func (m *TrayMenu) InsertSubmenu(int, string) (*TrayEntry, *TrayMenu) {
	return nil, nil
}
func (m *TrayMenu) AddSeparator() *TrayEntry               { return nil }
func (m *TrayMenu) InsertSeparator(int) *TrayEntry         { return nil }
func (e *TrayEntry) SetLabel(string)                       {}
func (e *TrayEntry) SetChecked(bool)                       {}
func (e *TrayEntry) Checked() bool                         { return false }
func (e *TrayEntry) SetEnabled(bool)                       {}
func (e *TrayEntry) Remove()                               {}

// Open connects to the DOM patcher started by web/qui.js.
func Open(title string, width, height int) *Ui {
	bridge := js.Global().Get("__qui")
	if bridge.IsUndefined() {
		panic("qui: start the wasm module through start() from web/qui.js")
	}
	u := newUi()
	u.bridge = bridge
	u.wake = make(chan struct{}, 1)
	u.onWake = js.FuncOf(func(js.Value, []js.Value) any {
		select {
		case u.wake <- struct{}{}:
		default:
		}
		return nil
	})
	bridge.Call("connect", title, u.onWake)
	return u
}

func (u *Ui) Close() { u.onWake.Release() }

// Wake requests a frame. It is safe to call from any goroutine.
func (u *Ui) Wake() { u.bridge.Call("invalidate") }

func (u *Ui) consumeBack() {}

// PickFiles does nothing in the browser yet.
func (u *Ui) PickFiles(bool) {}

// PickedFiles is always empty in the browser.
func (u *Ui) PickedFiles() []string { return nil }

// SetClipboard puts text on the system clipboard. Browsers only allow it while
// handling a user gesture.
func (u *Ui) SetClipboard(text string) {
	if clipboard := js.Global().Get("navigator").Get("clipboard"); !clipboard.IsUndefined() {
		clipboard.Call("writeText", text)
	}
}

// FilesHovering is always false in the browser.
func (u *Ui) FilesHovering() bool { return false }

// SelectedText returns the text selected on the page.
func (u *Ui) SelectedText() string {
	if sel := js.Global().Call("getSelection"); sel.Truthy() {
		return sel.Call("toString").String()
	}
	return ""
}

// SetSmoothScroll does nothing in the browser, which smooths the wheel itself.
func (u *Ui) SetSmoothScroll(bool) {}

func (u *Ui) submit() {
	if len(u.buf) > u.outCap {
		u.outCap = len(u.buf) * 2
		u.out = js.Global().Get("Uint8Array").New(u.outCap)
	}
	js.CopyBytesToJS(u.out, u.buf)
	u.bridge.Call("apply", u.out, len(u.buf))
}

func (u *Ui) wait() []byte {
	<-u.wake
	table := u.bridge.Call("takeResults")
	n := table.Length()
	if cap(u.results) < n {
		u.results = make([]byte, n*2)
	}
	u.results = u.results[:n]
	js.CopyBytesToGo(u.results, table)
	return u.results
}
