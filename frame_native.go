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

func (u *Ui) MinimizeWindow()  { u.app.WindowAction(0) }
func (u *Ui) ToggleMaximized() { u.app.WindowAction(1) }

// CloseWindow closes the window as its close button would, so Frame returns
// false.
func (u *Ui) CloseWindow() { u.app.WindowAction(2) }

func (u *Ui) WindowMaximized() bool { return u.app.WindowMaximized() }

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
