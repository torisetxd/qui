//go:build linux && !android && cgo

// Package sys handles the desktop plumbing native apps need on Linux: install
// files, a background service, a portal identity, and screen and microphone
// capture that keeps its permission between runs. It is separate from the UI
// package so a daemon can use it without linking SDL.
package sys

/*
// Not pkg-config for PipeWire: its cflags include -fno-strict-overflow, which cgo refuses.
#cgo CPPFLAGS: -I/usr/include/pipewire-0.3 -I/usr/include/spa-0.2 -D_REENTRANT
#cgo CXXFLAGS: -std=c++20 -fno-strict-aliasing
#cgo pkg-config: gio-2.0 gio-unix-2.0
#cgo LDFLAGS: -lpipewire-0.3 -lstdc++
#include <stdlib.h>
#include "qui_sys.h"
*/
import "C"

import (
	"errors"
	"time"
	"unsafe"
)

var (
	// ErrTimeout means nothing arrived before the timeout. Calling again is fine.
	ErrTimeout = errors.New("qui/sys: timed out")
	// ErrClosed means the session ended, usually because the user stopped sharing.
	ErrClosed = errors.New("qui/sys: session closed")
)

// App is one identity shared by the Wayland app id, the .desktop file, the
// portal registration, the systemd unit and the state directory.
type App struct {
	ID         string
	Name       string
	Comment    string
	Icon       string
	Categories string
	// Exec is an absolute path. Empty uses the running executable.
	Exec string
	// InstallAs copies the executable to ~/.local/bin under this name.
	InstallAs string
	Args      string
	Hidden    bool
	Service   *Service
}

// Service describes a systemd user service that runs the app in the background.
type Service struct {
	Args          string
	AlwaysRestart bool
	NeedsPipeWire bool
	// Headless starts with the user manager instead of the graphical session.
	Headless bool
}

type strings struct{ held []*C.char }

func (s *strings) add(v string) *C.char {
	if v == "" {
		return nil
	}
	p := C.CString(v)
	s.held = append(s.held, p)
	return p
}

func (s *strings) free() {
	for _, p := range s.held {
		C.free(unsafe.Pointer(p))
	}
}

func lastError() error { return errors.New("qui/sys: " + C.GoString(C.qsys_last_error())) }

func cBool(v bool) C.int {
	if v {
		return 1
	}
	return 0
}

func (a App) c(s *strings) C.qsys_app {
	out := C.qsys_app{
		id: s.add(a.ID), name: s.add(a.Name), comment: s.add(a.Comment), icon: s.add(a.Icon), categories: s.add(a.Categories),
		exec: s.add(a.Exec), install_as: s.add(a.InstallAs), args: s.add(a.Args), hidden: cBool(a.Hidden),
	}
	if a.Service != nil {
		out.service = 1
		out.service_args = s.add(a.Service.Args)
		out.service_always_restart = cBool(a.Service.AlwaysRestart)
		out.service_needs_pipewire = cBool(a.Service.NeedsPipeWire)
		out.service_headless = cBool(a.Service.Headless)
	}
	return out
}

// Install writes the .desktop file and, when set, the executable and the
// service unit. It is safe to call on every launch.
func Install(a App) error {
	var s strings
	defer s.free()
	app := a.c(&s)
	if C.qsys_install(&app) != 0 {
		return lastError()
	}
	return nil
}

// Uninstall removes everything Install wrote and any saved permissions.
func Uninstall(a App) error {
	var s strings
	defer s.free()
	app := a.c(&s)
	if C.qsys_uninstall(&app) != 0 {
		return lastError()
	}
	return nil
}

func service(id string, action C.int) error {
	cid := C.CString(id)
	defer C.free(unsafe.Pointer(cid))
	if C.qsys_service(cid, action) != 0 {
		return lastError()
	}
	return nil
}

func StartService(id string) error   { return service(id, C.QSYS_SERVICE_START) }
func StopService(id string) error    { return service(id, C.QSYS_SERVICE_STOP) }
func RestartService(id string) error { return service(id, C.QSYS_SERVICE_RESTART) }
func EnableService(id string) error  { return service(id, C.QSYS_SERVICE_ENABLE) }
func DisableService(id string) error { return service(id, C.QSYS_SERVICE_DISABLE) }

func ServiceActive(id string) bool {
	cid := C.CString(id)
	defer C.free(unsafe.Pointer(cid))
	return C.qsys_service_active(cid) != 0
}

// Register associates this process with an installed app id on the portal.
// OpenScreen does it for you.
func Register(id string) error {
	cid := C.CString(id)
	defer C.free(unsafe.Pointer(cid))
	if C.qsys_portal_register(cid) != 0 {
		return lastError()
	}
	return nil
}

type ScreenOptions struct {
	AppID string
	// Slot names the saved permission, so one app can hold several.
	Slot       string
	Windows    bool
	Multiple   bool
	ShowCursor bool
	// NoPersist forgets the permission when the session ends.
	NoPersist bool
	FPS       int
	// Timeout limits how long the permission dialog may stay open.
	Timeout time.Duration
}

type Stream struct {
	NodeID              uint32
	X, Y, Width, Height int
}

// Frame holds BGRx pixels, valid until the next Next on the same stream.
type Frame struct {
	Data                  []byte
	Width, Height, Stride int
	Sequence              uint64
	Time                  float64
}

type Screen struct{ p *C.qsys_screen }

// OpenScreen starts a capture session. The first run shows the permission
// dialog; later runs restore the saved permission without one.
func OpenScreen(o ScreenOptions) (*Screen, error) {
	var s strings
	defer s.free()
	options := C.qsys_screen_options{
		app_id: s.add(o.AppID), slot: s.add(o.Slot), windows: cBool(o.Windows), multiple: cBool(o.Multiple),
		show_cursor: cBool(o.ShowCursor), no_persist: cBool(o.NoPersist), fps: C.int(o.FPS), timeout_ms: C.int(o.Timeout.Milliseconds()),
	}
	p := C.qsys_screen_open(&options)
	if p == nil {
		return nil, lastError()
	}
	return &Screen{p}, nil
}

func (s *Screen) Close() { C.qsys_screen_close(s.p) }

// HadPermission reports whether a saved permission was presented to the portal.
func (s *Screen) HadPermission() bool { return C.qsys_screen_had_permission(s.p) != 0 }

func (s *Screen) Streams() []Stream {
	out := make([]Stream, int(C.qsys_screen_count(s.p)))
	for i := range out {
		var info C.qsys_stream_info
		C.qsys_screen_info(s.p, C.int(i), &info)
		out[i] = Stream{uint32(info.node_id), int(info.x), int(info.y), int(info.width), int(info.height)}
	}
	return out
}

// Next waits for the newest frame of a stream. Use one goroutine per stream.
func (s *Screen) Next(stream int, timeout time.Duration) (Frame, error) {
	var f C.qsys_frame
	switch C.qsys_screen_next(s.p, C.int(stream), &f, C.int(timeout.Milliseconds())) {
	case 1:
		return Frame{
			Data: unsafe.Slice((*byte)(unsafe.Pointer(f.data)), int(f.stride)*int(f.height)), Width: int(f.width), Height: int(f.height),
			Stride: int(f.stride), Sequence: uint64(f.sequence), Time: float64(f.time),
		}, nil
	case 0:
		return Frame{}, ErrTimeout
	default:
		return Frame{}, ErrClosed
	}
}

type AudioOptions struct {
	Name string
	// System captures what the default output is playing instead of the microphone.
	System bool
	// Target is a PipeWire node name or serial to capture instead of the default.
	Target   string
	Rate     int
	Channels int
}

type Audio struct {
	p        *C.qsys_audio
	channels int
}

// OpenAudio starts capturing. Rate and Channels default to 48000 and 2.
func OpenAudio(o AudioOptions) (*Audio, error) {
	var s strings
	defer s.free()
	options := C.qsys_audio_options{name: s.add(o.Name), system: cBool(o.System), target: s.add(o.Target), rate: C.int(o.Rate), channels: C.int(o.Channels)}
	p := C.qsys_audio_open(&options)
	if p == nil {
		return nil, lastError()
	}
	channels := o.Channels
	if channels <= 0 || channels > 2 {
		channels = 2
	}
	return &Audio{p, channels}, nil
}

func (a *Audio) Close() { C.qsys_audio_close(a.p) }

// Read fills out with interleaved samples and returns the number of frames and
// the monotonic time of the first one.
func (a *Audio) Read(out []float32, timeout time.Duration) (int, float64, error) {
	if len(out) < a.channels {
		return 0, 0, nil
	}
	var at C.double
	n := C.qsys_audio_read(a.p, (*C.float)(unsafe.Pointer(&out[0])), C.int(len(out)/a.channels), C.int(timeout.Milliseconds()), &at)
	switch {
	case n > 0:
		return int(n), float64(at), nil
	case n == 0:
		return 0, 0, ErrTimeout
	default:
		return 0, 0, ErrClosed
	}
}

// AudioSource is one capture source, named by its PipeWire node so it can be
// passed back as AudioOptions.Target.
type AudioSource struct {
	Name        string
	Description string
	// Monitor captures what a sink is playing instead of a microphone.
	Monitor bool
}

// AudioSources lists the capture sources. An empty result is not an error.
func AudioSources() ([]AudioSource, error) {
	const max = 64
	buffer := make([]C.qsys_audio_source, max)
	n := C.qsys_audio_sources(&buffer[0], max)
	if n < 0 {
		return nil, lastError()
	}
	out := make([]AudioSource, 0, int(n))
	for _, source := range buffer[:n] {
		out = append(out, AudioSource{
			Name:        C.GoString(&source.name[0]),
			Description: C.GoString(&source.description[0]),
			Monitor:     source.is_monitor != 0,
		})
	}
	return out, nil
}
