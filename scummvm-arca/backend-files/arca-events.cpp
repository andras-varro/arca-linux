/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// Forbidden symbol exceptions needed for file I/O (config loading) and
// device scanning (open/read/close/ioctl).
#define FORBIDDEN_SYMBOL_EXCEPTION_FILE
#define FORBIDDEN_SYMBOL_EXCEPTION_fopen
#define FORBIDDEN_SYMBOL_EXCEPTION_fclose
#define FORBIDDEN_SYMBOL_EXCEPTION_fgets
#define FORBIDDEN_SYMBOL_EXCEPTION_unistd_h

#include "backends/platform/arca/arca-events.h"
#include "backends/platform/arca/arca.h"
#include "backends/platform/arca/arca-graphics.h"
#include "backends/timer/default/default-timer.h"
#include "common/system.h"
#include "common/textconsole.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <errno.h>
#include <stdio.h>

// Device classification and the /dev/input/event* walk are common/input_scan.c's.
extern "C" {
#include "input_scan.h"
}

// =========================================================================
// Constructor / Destructor
// =========================================================================

ArcaEventSource::ArcaEventSource()
	: _touchInput(nullptr),
	  _touchInitialized(false),
	  _touchPhase(TOUCH_NONE),
	  _buttonDownSent(false),
	  _longPressFired(false),
	  _waitForRelease(false),
	  _prevOverlayVisible(false),
	  _lastTouchX(0),
	  _lastTouchY(0),
	  _touchStartTime(0),
	  _gameWidth(320),
	  _gameHeight(200),
	  _pendingHead(0),
	  _pendingCount(0),
	  // USB devices
	  _mouseNext(0),
	  _gamepadFd(-1),
	  _gamepadLayout(0),
	  // Keyboard
	  _modifierFlags(0),
	  // Mouse
	  _mouseX(400),
	  _mouseY(240),
	  _prevMouseButtons(0),
	  // Gamepad
	  _gamepadAxisX(0),
	  _gamepadAxisY(0),
	  _gamepadHatX(0),
	  _gamepadHatY(0),
	  _gamepadAxisMin(-32768),
	  _gamepadAxisMax(32767),
	  _gamepadAxisCenter(0),
	  _prevGamepadButtons(0),
	  _lastCursorMove(0) {

	memset(_cornerTaps, 0, sizeof(_cornerTaps));
	memset(_pending,    0, sizeof(_pending));
	for (int i = 0; i < MAX_KEYBOARDS; i++) { _keyboardFds[i] = -1; _keyboardNodes[i] = -1; }
	for (int i = 0; i < MAX_MICE; i++)      { _mouseFds[i] = -1;    _mouseNodes[i] = -1; }

	_screenW = screen_base_width;
	_screenH = screen_base_height;

	memset(&_nodeGate, 0, sizeof(_nodeGate));   // no check yet: the first scan sets the baseline
	loadInputConfig();
	initTouch();
	scanInputDevices();
}

ArcaEventSource::~ArcaEventSource() {
	closeTouch();
	closeInputDevices();
}

// =========================================================================
// Touch initialization (unchanged from original)
// =========================================================================

void ArcaEventSource::initTouch() {
	if (_touchInitialized)
		return;

	_touchInput = (TouchInput *)malloc(sizeof(TouchInput));
	if (!_touchInput) {
		warning("Failed to allocate touch input structure");
		return;
	}

	if (touch_init(_touchInput, "/dev/input/touchscreen0") < 0) {
		warning("Failed to initialize touch input - device may not be available");
		free(_touchInput);
		_touchInput = nullptr;
		return;
	}

	// Coordinate geometry. This runs before the framebuffer exists, so it picks
	// up the pre-bezel defaults; syncScreenGeometry() corrects it from initSize().
	touch_set_screen_size(_touchInput, screen_base_width, screen_base_height);

	// Load touch calibration if available
	if (touch_load_calibration(_touchInput, "/etc/touch_calibration.conf") == 0) {
		touch_enable_calibration(_touchInput, true);
		debug("Arca: touch calibration loaded");
	} else {
		debug("Arca: no calibration file, using defaults");
	}

	_touchInitialized = true;
	debug("Arca touch input initialized");
}

void ArcaEventSource::closeTouch() {
	if (_touchInitialized && _touchInput) {
		touch_close(_touchInput);
		free(_touchInput);
		_touchInput = nullptr;
		_touchInitialized = false;
	}
}

// =========================================================================
// USB Input Device Scanning and Management
// =========================================================================

// input_scan() already refuses the Panjit names; this copy has always also
// refused any node that calls itself a touchscreen — the touchscreen is
// touch_input.c's, opened by initTouch() — and keeps doing so.
static const char *const kTouchscreenNames[] = { "TouchScreen", "touchscreen", nullptr };

// BUG-INPUT-004 FIX: Log permission errors so we can diagnose
// "no input" issues caused by /dev/input/event* permissions.
static void warnOpenFailed(const char *path, int err, void *) {
	if (err == EACCES || err == EPERM)
		warning("Arca: cannot open %s — permission denied (run as root or add to 'input' group)", path);
}

void ArcaEventSource::scanInputDevices() {
	static const char kEventPrefix[] = "/dev/input/event";
	int totalBefore = countOpen(_keyboardFds, MAX_KEYBOARDS) + countOpen(_mouseFds, MAX_MICE) +
	                  (_gamepadFd >= 0 ? 1 : 0);
	// Fingerprint first, so a node that appears during the walk differs from
	// this baseline and the next check catches it.  No baseline yet means this
	// is the constructor's scan, which always logs its summary.
	bool firstScan = !_nodeGate.have_sig;
	input_sig_gate_baseline(&_nodeGate, input_node_sig());

	// Held keyboards and mice are listed first: input_scan() skips them without
	// opening them and counts them against the caps.  A held pad is not listed;
	// its cap drops to 0 instead, so its node is opened, classified and closed.
	InputNode nodes[MAX_KEYBOARDS + MAX_MICE + 1];
	int n = 0;
	for (int i = 0; i < MAX_KEYBOARDS; i++) {
		if (_keyboardFds[i] < 0) continue;
		snprintf(nodes[n].path, sizeof(nodes[n].path), "%s%d", kEventPrefix, _keyboardNodes[i]);
		nodes[n].fd = _keyboardFds[i];
		nodes[n].kind = INPUT_KIND_KEYBOARD;
		nodes[n].name[0] = '\0';
		n++;
	}
	for (int i = 0; i < MAX_MICE; i++) {
		if (_mouseFds[i] < 0) continue;
		snprintf(nodes[n].path, sizeof(nodes[n].path), "%s%d", kEventPrefix, _mouseNodes[i]);
		nodes[n].fd = _mouseFds[i];
		nodes[n].kind = INPUT_KIND_MOUSE;
		nodes[n].name[0] = '\0';
		n++;
	}
	const int held = n;

	int cap[INPUT_KIND_COUNT] = {0};
	cap[INPUT_KIND_KEYBOARD] = MAX_KEYBOARDS;
	cap[INPUT_KIND_MOUSE]    = MAX_MICE;
	cap[INPUT_KIND_PAD]      = (_gamepadFd < 0) ? 1 : 0;
	InputScanOpts opts = { kTouchscreenNames, warnOpenFailed, nullptr };
	n = input_scan_with(nodes, n, (int)(sizeof(nodes) / sizeof(nodes[0])), cap, &opts);

	for (int i = held; i < n; i++) {
		const InputNode &nd = nodes[i];
		int node = atoi(nd.path + sizeof(kEventPrefix) - 1);
		if (nd.kind == INPUT_KIND_PAD) {
			_gamepadFd = nd.fd;
			_gamepadLayout = nd.pad_layout;
			loadGamepadAxisCalibration();
			warning("Arca: Detected gamepad '%s' at %s", nd.name, nd.path);
		} else if (nd.kind == INPUT_KIND_KEYBOARD &&
		           addToSlot(_keyboardFds, _keyboardNodes, MAX_KEYBOARDS, nd.fd, node)) {
			warning("Arca: Detected keyboard '%s' at %s", nd.name, nd.path);
		} else if (nd.kind == INPUT_KIND_MOUSE &&
		           addToSlot(_mouseFds, _mouseNodes, MAX_MICE, nd.fd, node)) {
			warning("Arca: Detected mouse '%s' at %s", nd.name, nd.path);
			// A keyboard+touchpad combo is one node, classified MOUSE, and
			// pollMouseFd() reads only its buttons.  Open it again as a
			// keyboard: evdev gives every open file every event, so each
			// reader takes its own half and keeps its own disconnect.
			if (nd.keys) {
				int kfd = open(nd.path, O_RDONLY | O_NONBLOCK);
				if (kfd >= 0 && addToSlot(_keyboardFds, _keyboardNodes, MAX_KEYBOARDS, kfd, node))
					warning("Arca: Detected keyboard '%s' at %s (same node)", nd.name, nd.path);
				else if (kfd >= 0)
					close(kfd);
			}
		} else {
			close(nd.fd);  // unreachable: the caps match the slot counts
		}
	}

	// BUG-INPUT-004 FIX: Log a summary after scanning so we know what was
	// found (or not found).  This is essential for diagnosing "no input" issues.
	// A rescan follows any /dev/input change, including nodes this never opens
	// (the touchscreen), so after the first scan it logs only when the set changed.
	int total = countOpen(_keyboardFds, MAX_KEYBOARDS) + countOpen(_mouseFds, MAX_MICE) +
	            (_gamepadFd >= 0 ? 1 : 0);
	if (!firstScan && total == totalBefore)
		return;
	warning("Arca: device scan complete — keyboards=%d mice=%d gamepad=%s",
	      countOpen(_keyboardFds, MAX_KEYBOARDS),
	      countOpen(_mouseFds, MAX_MICE),
	      (_gamepadFd >= 0  ? "YES" : "no"));
}

int ArcaEventSource::countOpen(const int *fds, int n) {
	int c = 0;
	for (int i = 0; i < n; i++)
		if (fds[i] >= 0) c++;
	return c;
}

bool ArcaEventSource::addToSlot(int *fds, int *nodes, int n, int fd, int node) {
	for (int i = 0; i < n; i++) {
		if (fds[i] < 0) {
			fds[i] = fd;
			nodes[i] = node;
			return true;
		}
	}
	return false;  // every slot taken; the caller closes fd
}

void ArcaEventSource::closeInputDevices() {
	for (int i = 0; i < MAX_KEYBOARDS; i++)
		if (_keyboardFds[i] >= 0) { close(_keyboardFds[i]); _keyboardFds[i] = -1; _keyboardNodes[i] = -1; }
	for (int i = 0; i < MAX_MICE; i++)
		if (_mouseFds[i] >= 0)    { close(_mouseFds[i]);    _mouseFds[i] = -1;    _mouseNodes[i] = -1; }
	if (_gamepadFd >= 0)  { close(_gamepadFd);  _gamepadFd = -1; }
	_gamepadLayout = INPUT_PAD_NATIVE;
}

void ArcaEventSource::loadGamepadAxisCalibration() {
	if (_gamepadFd < 0) return;

	struct input_absinfo info;
	if (ioctl(_gamepadFd, EVIOCGABS(_gamepadMap.stickXAxis), &info) == 0) {
		_gamepadAxisMin = info.minimum;
		_gamepadAxisMax = info.maximum;
		// BUG-INPUT-004 FIX: Use computed midpoint instead of info.value.
		// info.value is the stick position at the moment of the ioctl call;
		// if the stick is not perfectly centered the cursor will drift.
		// Same fix pattern as BUG-INPUT-003 in native_apps gamepad.c.
		_gamepadAxisCenter = (info.minimum + info.maximum) / 2;
		debug("Arca: gamepad axis range [%d..%d], center=%d (computed midpoint)",
		      _gamepadAxisMin, _gamepadAxisMax, _gamepadAxisCenter);
	}
}

// =========================================================================
// Gesture detection (unchanged from original)
// =========================================================================

// The touch-safe rectangle, from the graphics manager so there is one source of
// truth for it. Falls back to the whole logical screen before the graphics
// manager exists (and on a panel whose reach has never been swept, where the
// safe rect IS the whole screen anyway).
void ArcaEventSource::safeRect(int &x, int &y, int &w, int &h) const {
	OSystem_Arca *system = arcaSystem();
	if (system && system->getGraphicsManager()) {
		((ArcaGraphicsManager *)system->getGraphicsManager())
			->getSafeRect(x, y, w, h);
		return;
	}
	x = 0;
	y = 0;
	w = _screenW;
	h = _screenH;
}

// Where the virtual cursor (USB mouse, gamepad stick) is allowed to go: exactly
// the rectangle the current content occupies. In overlay mode that is the
// touch-safe rectangle; in game mode it is the picture, wherever getScalingInfo()
// put it — which matters for the arca_content_area=visible opt-out, where the
// picture deliberately extends past the safe rect and a mouse can still reach it
// even though a finger cannot.
void ArcaEventSource::cursorBounds(int &x, int &y, int &w, int &h) const {
	OSystem_Arca *system = arcaSystem();
	ArcaGraphicsManager *gfx = (system && system->getGraphicsManager())
		? (ArcaGraphicsManager *)system->getGraphicsManager() : nullptr;
	if (gfx && !gfx->isOverlayVisible()) {
		int scaledW, scaledH, offX, offY;
		gfx->getScalingInfo(scaledW, scaledH, offX, offY);
		if (scaledW > 0 && scaledH > 0) {
			x = offX;
			y = offY;
			w = scaledW;
			h = scaledH;
			return;
		}
	}
	safeRect(x, y, w, h);
}

ArcaEventSource::Corner ArcaEventSource::cornerFor(int x, int y) const {
	const int ZONE = 80;
	// Measured from the touch-safe rectangle, not the surface: a gesture zone is
	// a touch target, and the outermost rows/columns cannot be pressed at all.
	int sx, sy, sw, sh;
	safeRect(sx, sy, sw, sh);
	if (y > sy + sh - ZONE) {
		if (x < sx + ZONE)      return CORNER_BL;
		if (x > sx + sw - ZONE) return CORNER_BR;
	}
	return CORNER_COUNT;
}

void ArcaEventSource::pushEvent(const Common::Event &e) {
	if (_pendingCount >= MAX_PENDING)
		return;
	int slot = (_pendingHead + _pendingCount) % MAX_PENDING;
	_pending[slot] = e;
	_pendingCount++;
}

void ArcaEventSource::checkGestures(int touchX, int touchY, uint32 now) {
	Corner c = cornerFor(touchX, touchY);
	if (c == CORNER_COUNT)
		return;

	const uint32 WINDOW = 1200;
	CornerTaps &ct = _cornerTaps[c];

	while (ct.count > 0) {
		if (now - ct.timestamps[0] > WINDOW) {
			for (int i = 0; i < ct.count - 1; i++)
				ct.timestamps[i] = ct.timestamps[i + 1];
			ct.count--;
		} else {
			break;
		}
	}

	if (ct.count < 3)
		ct.timestamps[ct.count] = now;
	else {
		ct.timestamps[0] = ct.timestamps[1];
		ct.timestamps[1] = ct.timestamps[2];
		ct.timestamps[2] = now;
	}
	ct.count = (ct.count < 3) ? ct.count + 1 : 3;

	if (ct.count == 3 && (now - ct.timestamps[0]) <= WINDOW) {
		ct.count = 0;
		_waitForRelease = true;
		debug("Gesture: triple-tap corner %d", (int)c);

		if (c == CORNER_BR) {
			debug("Gesture: opening GMM via Ctrl+F5");
			Common::Event ctrlDown, f5Down, f5Up, ctrlUp;
			ctrlDown.type        = Common::EVENT_KEYDOWN;
			ctrlDown.kbd.keycode = Common::KEYCODE_LCTRL;
			ctrlDown.kbd.flags   = Common::KBD_CTRL;
			ctrlDown.kbd.ascii   = 0;
			f5Down.type          = Common::EVENT_KEYDOWN;
			f5Down.kbd.keycode   = Common::KEYCODE_F5;
			f5Down.kbd.flags     = Common::KBD_CTRL;
			f5Down.kbd.ascii     = 0;
			f5Up = f5Down;
			f5Up.type            = Common::EVENT_KEYUP;
			ctrlUp = ctrlDown;
			ctrlUp.type          = Common::EVENT_KEYUP;
			ctrlUp.kbd.flags     = 0;
			pushEvent(ctrlDown);
			pushEvent(f5Down);
			pushEvent(f5Up);
			pushEvent(ctrlUp);
		} else if (c == CORNER_BL) {
			debug("Gesture: opening virtual keyboard");
			OSystem_Arca *sys = arcaSystem();
			if (sys)
				sys->showVirtualKeyboard();
		}
	}
}

void ArcaEventSource::syncScreenGeometry() {
	_screenW = screen_base_width;
	_screenH = screen_base_height;
	if (_touchInitialized && _touchInput) {
		// Also refreshes the panel size and bezel viewport from the globals.
		touch_set_screen_size(_touchInput, _screenW, _screenH);
	}
	debug("Arca: visible screen %dx%d", _screenW, _screenH);
}

void ArcaEventSource::setGameScreenSize(int width, int height, int offsetX, int offsetY) {
	_gameWidth = width;
	_gameHeight = height;
	debug("Game screen size set to %dx%d at offset (%d, %d)", width, height, offsetX, offsetY);
}

void ArcaEventSource::transformCoordinates(int touchX, int touchY, int &gameX, int &gameY) {
	OSystem_Arca *system = arcaSystem();
	ArcaGraphicsManager *gfx = (system && system->getGraphicsManager())
		? (ArcaGraphicsManager *)system->getGraphicsManager() : nullptr;
	if (gfx && gfx->isOverlayVisible()) {
		// Overlay space is the touch-safe rectangle (the GUI is all buttons, so
		// it is confined to what a finger can reach), so subtract its origin and
		// clamp to the overlay's own size rather than the screen's.
		int sx, sy, sw, sh;
		safeRect(sx, sy, sw, sh);
		gameX = touchX - sx;
		gameY = touchY - sy;
		if (gameX < 0) gameX = 0;
		if (gameY < 0) gameY = 0;
		if (gameX >= sw) gameX = sw - 1;
		if (gameY >= sh) gameY = sh - 1;
	} else {
		int scaledW = _gameWidth, scaledH = _gameHeight;
		int fbOffsetX = 0, fbOffsetY = 0;
		if (gfx) {
			// The offsets already include the content rectangle's origin.
			gfx->getScalingInfo(scaledW, scaledH, fbOffsetX, fbOffsetY);
		}
		int relX = touchX - fbOffsetX;
		int relY = touchY - fbOffsetY;
		if (scaledW > 0 && scaledH > 0) {
			gameX = relX * _gameWidth  / scaledW;
			gameY = relY * _gameHeight / scaledH;
		} else {
			gameX = relX;
			gameY = relY;
		}
		if (gameX < 0) gameX = 0;
		if (gameY < 0) gameY = 0;
		if (gameX >= _gameWidth)  gameX = _gameWidth  - 1;
		if (gameY >= _gameHeight) gameY = _gameHeight - 1;
	}
}

// =========================================================================
// Config file loading
// =========================================================================

// The file is parsed by input_config_load() (common/input_scan.c), the one
// parser every component calls.  The shared defaults are applied first, so
// this is also where every input default comes from — the dead zone included,
// which used to be a ScummVM-only constant.
void ArcaEventSource::loadInputConfig() {
	InputConfig cfg;
	input_config_defaults(&cfg);
	int applied = input_config_load(&cfg, INPUT_CONFIG_PATH);

	_mouseSensitivity   = cfg.mouse_sensitivity;
	_mouseAcceleration  = cfg.mouse_acceleration;
	_mouseLowThreshold  = cfg.mouse_low_threshold;
	_mouseHighThreshold = cfg.mouse_high_threshold;
	_gamepadDeadzonePct = cfg.gamepad_deadzone;

	_gamepadMap.btnSouth   = cfg.gamepad_btn_jump;
	_gamepadMap.btnEast    = cfg.gamepad_btn_run;
	_gamepadMap.btnWest    = cfg.gamepad_btn_action;
	_gamepadMap.btnNorth   = cfg.gamepad_btn_north;
	_gamepadMap.btnStart   = cfg.gamepad_btn_pause;
	_gamepadMap.btnSelect  = cfg.gamepad_btn_back;
	_gamepadMap.btnTL      = cfg.gamepad_btn_tl;
	_gamepadMap.btnTR      = cfg.gamepad_btn_tr;
	_gamepadMap.hatXAxis   = cfg.gamepad_hat_x;
	_gamepadMap.hatYAxis   = cfg.gamepad_hat_y;
	_gamepadMap.stickXAxis = cfg.gamepad_stick_lx;
	_gamepadMap.stickYAxis = cfg.gamepad_stick_ly;

	if (applied < 0)
		debug("Arca: no input config at %s (using defaults)", INPUT_CONFIG_PATH);
	else
		debug("Arca: loaded input config, %d settings (mouse sens=%.1f accel=%.1f "
		      "thresh=%d/%d, deadzone=%d%%)", applied, _mouseSensitivity, _mouseAcceleration,
		      _mouseLowThreshold, _mouseHighThreshold, _gamepadDeadzonePct);
}

// =========================================================================
// Keyboard: Linux KEY_* to ScummVM KEYCODE_* mapping
// =========================================================================

ArcaEventSource::KeyMapping ArcaEventSource::mapLinuxKey(int linuxKeyCode, byte modifiers) {
	KeyMapping m;
	m.keycode = Common::KEYCODE_INVALID;
	m.ascii = 0;
	m.shiftAscii = 0;

	switch (linuxKeyCode) {
	case 1:  m.keycode = Common::KEYCODE_ESCAPE;    m.ascii = 27;  break;
	case 2:  m.keycode = Common::KEYCODE_1; m.ascii = '1'; m.shiftAscii = '!'; break;
	case 3:  m.keycode = Common::KEYCODE_2; m.ascii = '2'; m.shiftAscii = '@'; break;
	case 4:  m.keycode = Common::KEYCODE_3; m.ascii = '3'; m.shiftAscii = '#'; break;
	case 5:  m.keycode = Common::KEYCODE_4; m.ascii = '4'; m.shiftAscii = '$'; break;
	case 6:  m.keycode = Common::KEYCODE_5; m.ascii = '5'; m.shiftAscii = '%'; break;
	case 7:  m.keycode = Common::KEYCODE_6; m.ascii = '6'; m.shiftAscii = '^'; break;
	case 8:  m.keycode = Common::KEYCODE_7; m.ascii = '7'; m.shiftAscii = '&'; break;
	case 9:  m.keycode = Common::KEYCODE_8; m.ascii = '8'; m.shiftAscii = '*'; break;
	case 10: m.keycode = Common::KEYCODE_9; m.ascii = '9'; m.shiftAscii = '('; break;
	case 11: m.keycode = Common::KEYCODE_0; m.ascii = '0'; m.shiftAscii = ')'; break;
	case 12: m.keycode = Common::KEYCODE_MINUS;  m.ascii = '-'; m.shiftAscii = '_'; break;
	case 13: m.keycode = Common::KEYCODE_EQUALS; m.ascii = '='; m.shiftAscii = '+'; break;
	case 14: m.keycode = Common::KEYCODE_BACKSPACE; m.ascii = 8;  break;
	case 15: m.keycode = Common::KEYCODE_TAB;       m.ascii = 9;  break;
	case 16: m.keycode = Common::KEYCODE_q; m.ascii = 'q'; m.shiftAscii = 'Q'; break;
	case 17: m.keycode = Common::KEYCODE_w; m.ascii = 'w'; m.shiftAscii = 'W'; break;
	case 18: m.keycode = Common::KEYCODE_e; m.ascii = 'e'; m.shiftAscii = 'E'; break;
	case 19: m.keycode = Common::KEYCODE_r; m.ascii = 'r'; m.shiftAscii = 'R'; break;
	case 20: m.keycode = Common::KEYCODE_t; m.ascii = 't'; m.shiftAscii = 'T'; break;
	case 21: m.keycode = Common::KEYCODE_y; m.ascii = 'y'; m.shiftAscii = 'Y'; break;
	case 22: m.keycode = Common::KEYCODE_u; m.ascii = 'u'; m.shiftAscii = 'U'; break;
	case 23: m.keycode = Common::KEYCODE_i; m.ascii = 'i'; m.shiftAscii = 'I'; break;
	case 24: m.keycode = Common::KEYCODE_o; m.ascii = 'o'; m.shiftAscii = 'O'; break;
	case 25: m.keycode = Common::KEYCODE_p; m.ascii = 'p'; m.shiftAscii = 'P'; break;
	case 26: m.keycode = Common::KEYCODE_LEFTBRACKET;  m.ascii = '['; m.shiftAscii = '{'; break;
	case 27: m.keycode = Common::KEYCODE_RIGHTBRACKET; m.ascii = ']'; m.shiftAscii = '}'; break;
	case 28: m.keycode = Common::KEYCODE_RETURN; m.ascii = 13; break;
	case 29: m.keycode = Common::KEYCODE_LCTRL;  break;
	case 30: m.keycode = Common::KEYCODE_a; m.ascii = 'a'; m.shiftAscii = 'A'; break;
	case 31: m.keycode = Common::KEYCODE_s; m.ascii = 's'; m.shiftAscii = 'S'; break;
	case 32: m.keycode = Common::KEYCODE_d; m.ascii = 'd'; m.shiftAscii = 'D'; break;
	case 33: m.keycode = Common::KEYCODE_f; m.ascii = 'f'; m.shiftAscii = 'F'; break;
	case 34: m.keycode = Common::KEYCODE_g; m.ascii = 'g'; m.shiftAscii = 'G'; break;
	case 35: m.keycode = Common::KEYCODE_h; m.ascii = 'h'; m.shiftAscii = 'H'; break;
	case 36: m.keycode = Common::KEYCODE_j; m.ascii = 'j'; m.shiftAscii = 'J'; break;
	case 37: m.keycode = Common::KEYCODE_k; m.ascii = 'k'; m.shiftAscii = 'K'; break;
	case 38: m.keycode = Common::KEYCODE_l; m.ascii = 'l'; m.shiftAscii = 'L'; break;
	case 39: m.keycode = Common::KEYCODE_SEMICOLON; m.ascii = ';'; m.shiftAscii = ':'; break;
	case 40: m.keycode = Common::KEYCODE_QUOTE;     m.ascii = '\''; m.shiftAscii = '"'; break;
	case 41: m.keycode = Common::KEYCODE_BACKQUOTE; m.ascii = '`'; m.shiftAscii = '~'; break;
	case 42: m.keycode = Common::KEYCODE_LSHIFT; break;
	case 43: m.keycode = Common::KEYCODE_BACKSLASH; m.ascii = '\\'; m.shiftAscii = '|'; break;
	case 44: m.keycode = Common::KEYCODE_z; m.ascii = 'z'; m.shiftAscii = 'Z'; break;
	case 45: m.keycode = Common::KEYCODE_x; m.ascii = 'x'; m.shiftAscii = 'X'; break;
	case 46: m.keycode = Common::KEYCODE_c; m.ascii = 'c'; m.shiftAscii = 'C'; break;
	case 47: m.keycode = Common::KEYCODE_v; m.ascii = 'v'; m.shiftAscii = 'V'; break;
	case 48: m.keycode = Common::KEYCODE_b; m.ascii = 'b'; m.shiftAscii = 'B'; break;
	case 49: m.keycode = Common::KEYCODE_n; m.ascii = 'n'; m.shiftAscii = 'N'; break;
	case 50: m.keycode = Common::KEYCODE_m; m.ascii = 'm'; m.shiftAscii = 'M'; break;
	case 51: m.keycode = Common::KEYCODE_COMMA;  m.ascii = ','; m.shiftAscii = '<'; break;
	case 52: m.keycode = Common::KEYCODE_PERIOD; m.ascii = '.'; m.shiftAscii = '>'; break;
	case 53: m.keycode = Common::KEYCODE_SLASH;  m.ascii = '/'; m.shiftAscii = '?'; break;
	case 54: m.keycode = Common::KEYCODE_RSHIFT; break;
	case 56: m.keycode = Common::KEYCODE_LALT; break;
	case 57: m.keycode = Common::KEYCODE_SPACE; m.ascii = ' '; break;
	case 58: m.keycode = Common::KEYCODE_CAPSLOCK; break;
	case 59: m.keycode = Common::KEYCODE_F1;  m.ascii = Common::ASCII_F1;  break;
	case 60: m.keycode = Common::KEYCODE_F2;  m.ascii = Common::ASCII_F2;  break;
	case 61: m.keycode = Common::KEYCODE_F3;  m.ascii = Common::ASCII_F3;  break;
	case 62: m.keycode = Common::KEYCODE_F4;  m.ascii = Common::ASCII_F4;  break;
	case 63: m.keycode = Common::KEYCODE_F5;  m.ascii = Common::ASCII_F5;  break;
	case 64: m.keycode = Common::KEYCODE_F6;  m.ascii = Common::ASCII_F6;  break;
	case 65: m.keycode = Common::KEYCODE_F7;  m.ascii = Common::ASCII_F7;  break;
	case 66: m.keycode = Common::KEYCODE_F8;  m.ascii = Common::ASCII_F8;  break;
	case 67: m.keycode = Common::KEYCODE_F9;  m.ascii = Common::ASCII_F9;  break;
	case 68: m.keycode = Common::KEYCODE_F10; m.ascii = Common::ASCII_F10; break;
	case 69: m.keycode = Common::KEYCODE_NUMLOCK;  break;
	case 70: m.keycode = Common::KEYCODE_SCROLLOCK; break;
	case 71: m.keycode = Common::KEYCODE_KP7; m.ascii = '7'; break;
	case 72: m.keycode = Common::KEYCODE_KP8; m.ascii = '8'; break;
	case 73: m.keycode = Common::KEYCODE_KP9; m.ascii = '9'; break;
	case 74: m.keycode = Common::KEYCODE_KP_MINUS; m.ascii = '-'; break;
	case 75: m.keycode = Common::KEYCODE_KP4; m.ascii = '4'; break;
	case 76: m.keycode = Common::KEYCODE_KP5; m.ascii = '5'; break;
	case 77: m.keycode = Common::KEYCODE_KP6; m.ascii = '6'; break;
	case 78: m.keycode = Common::KEYCODE_KP_PLUS; m.ascii = '+'; break;
	case 79: m.keycode = Common::KEYCODE_KP1; m.ascii = '1'; break;
	case 80: m.keycode = Common::KEYCODE_KP2; m.ascii = '2'; break;
	case 81: m.keycode = Common::KEYCODE_KP3; m.ascii = '3'; break;
	case 82: m.keycode = Common::KEYCODE_KP0; m.ascii = '0'; break;
	case 83: m.keycode = Common::KEYCODE_KP_PERIOD; m.ascii = '.'; break;
	case 87: m.keycode = Common::KEYCODE_F11; m.ascii = Common::ASCII_F11; break;
	case 88: m.keycode = Common::KEYCODE_F12; m.ascii = Common::ASCII_F12; break;
	case 96: m.keycode = Common::KEYCODE_KP_ENTER;  m.ascii = 13;  break;
	case 97: m.keycode = Common::KEYCODE_RCTRL; break;
	case 98: m.keycode = Common::KEYCODE_KP_DIVIDE; m.ascii = '/'; break;
	case 100: m.keycode = Common::KEYCODE_RALT; break;
	case 102: m.keycode = Common::KEYCODE_HOME;     break;
	case 103: m.keycode = Common::KEYCODE_UP;    break;
	case 104: m.keycode = Common::KEYCODE_PAGEUP;   break;
	case 105: m.keycode = Common::KEYCODE_LEFT;  break;
	case 106: m.keycode = Common::KEYCODE_RIGHT; break;
	case 107: m.keycode = Common::KEYCODE_END;      break;
	case 108: m.keycode = Common::KEYCODE_DOWN;  break;
	case 109: m.keycode = Common::KEYCODE_PAGEDOWN; break;
	case 110: m.keycode = Common::KEYCODE_INSERT;   break;
	case 111: m.keycode = Common::KEYCODE_DELETE; m.ascii = 127; break;
	case 125: m.keycode = Common::KEYCODE_LSUPER; break;
	case 126: m.keycode = Common::KEYCODE_RSUPER; break;
	default: break;
	}

	// Apply shift to ascii
	if ((modifiers & Common::KBD_SHIFT) && m.shiftAscii != 0)
		m.ascii = m.shiftAscii;

	return m;
}

// =========================================================================
// Keyboard polling
// =========================================================================

bool ArcaEventSource::pollKeyboard(Common::Event &event) {
	// Every held keyboard feeds the same key stream and modifier state.
	for (int i = 0; i < MAX_KEYBOARDS; i++)
		if (pollKeyboardFd(i, event)) return true;
	return false;
}

bool ArcaEventSource::pollKeyboardFd(int slot, Common::Event &event) {
	int &fd = _keyboardFds[slot];
	if (fd < 0) return false;

	// BUG-INPUT-004 FIX: Clear errno before the read loop so stale values
	// (e.g. EAGAIN from a previous poll) don't trigger false disconnect detection.
	errno = 0;

	// Per-slot state outlives this call: the loop returns on the first event,
	// so a drop and the packet it tears can straddle several calls, and the keys
	// believed held are what a resync has to compare the kernel's levels against.
	static InputSynDrop sd[MAX_KEYBOARDS];
	static unsigned long held[MAX_KEYBOARDS][INPUT_SCAN_NLONGS(KEY_MAX + 1)];
	const int LB = 8 * (int)sizeof(unsigned long);

	struct input_event ev;
	while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (input_syn_drop_skip(&sd[slot], &ev)) continue;
		if (ev.type != EV_KEY) continue;

		// value: 0=release, 1=press, 2=auto-repeat (ignored — ScummVM repeats internally)
		if (ev.value == 2) continue;
		bool pressed = (ev.value == 1);
		if (ev.code <= KEY_MAX) {
			if (pressed) held[slot][ev.code / LB] |=  (1UL << (ev.code % LB));
			else         held[slot][ev.code / LB] &= ~(1UL << (ev.code % LB));
		}

		// Update modifier state
		switch (ev.code) {
		case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
			if (pressed) _modifierFlags |= Common::KBD_SHIFT;
			else         _modifierFlags &= ~Common::KBD_SHIFT;
			break;
		case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
			if (pressed) _modifierFlags |= Common::KBD_CTRL;
			else         _modifierFlags &= ~Common::KBD_CTRL;
			break;
		case KEY_LEFTALT: case KEY_RIGHTALT:
			if (pressed) _modifierFlags |= Common::KBD_ALT;
			else         _modifierFlags &= ~Common::KBD_ALT;
			break;
		default: break;
		}

		KeyMapping km = mapLinuxKey(ev.code, _modifierFlags);
		if (km.keycode == Common::KEYCODE_INVALID) continue;

		event.type = pressed ? Common::EVENT_KEYDOWN : Common::EVENT_KEYUP;
		event.kbdRepeat = false;
		event.kbd.keycode = km.keycode;
		event.kbd.ascii = km.ascii;
		event.kbd.flags = _modifierFlags;
		return true;
	}

	// SYN_DROPPED (input_scan.h): a release may have been discarded with the
	// overflow.  Every key held here that the kernel says is up gets its KEYUP,
	// so a lost release leaves the key up instead of stuck down.  The ring is
	// drained at this point, so the snapshot and the events agree.
	if (sd[slot].resync && errno != ENODEV && errno != EBADF) {
		sd[slot].resync = false;
		unsigned long keys[INPUT_SCAN_NLONGS(KEY_MAX + 1)];
		memset(keys, 0, sizeof(keys));
		if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
			bool first = true;
			Common::Event out;
			for (int code = 0; code <= KEY_MAX; code++) {
				if (!(held[slot][code / LB] & (1UL << (code % LB)))) continue;
				if (input_caps_test(keys, code)) continue;
				held[slot][code / LB] &= ~(1UL << (code % LB));
				switch (code) {
				case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: _modifierFlags &= ~Common::KBD_SHIFT; break;
				case KEY_LEFTCTRL:  case KEY_RIGHTCTRL:  _modifierFlags &= ~Common::KBD_CTRL;  break;
				case KEY_LEFTALT:   case KEY_RIGHTALT:   _modifierFlags &= ~Common::KBD_ALT;   break;
				default: break;
				}
				KeyMapping km = mapLinuxKey(code, _modifierFlags);
				if (km.keycode == Common::KEYCODE_INVALID) continue;
				out.type = Common::EVENT_KEYUP;
				out.kbdRepeat = false;
				out.kbd.keycode = km.keycode;
				out.kbd.ascii = km.ascii;
				out.kbd.flags = _modifierFlags;
				if (first) { event = out; first = false; }
				else pushEvent(out);
			}
			if (!first) return true;
		}
	}

	// Check for disconnect
	if (errno == ENODEV || errno == EBADF) {
		memset(held[slot], 0, sizeof(held[slot]));
		sd[slot].dropping = sd[slot].resync = false;
		debug("Arca: keyboard disconnected (event%d)", _keyboardNodes[slot]);
		close(fd);
		fd = -1;
		_keyboardNodes[slot] = -1;
		_modifierFlags = 0;
	}
	return false;
}

// =========================================================================
// Mouse polling with 3-tier acceleration
// =========================================================================

bool ArcaEventSource::pollMouse(Common::Event &event) {
	// Every held mouse moves the same cursor and shares one button state.
	// The start slot rotates so a mouse that always has motion queued cannot
	// starve the others.
	for (int k = 0; k < MAX_MICE; k++) {
		int slot = (_mouseNext + k) % MAX_MICE;
		if (pollMouseFd(slot, event)) {
			_mouseNext = (slot + 1) % MAX_MICE;
			return true;
		}
	}
	return false;
}

bool ArcaEventSource::pollMouseFd(int slot, Common::Event &event) {
	int &fd = _mouseFds[slot];
	if (fd < 0) return false;

	// BUG-INPUT-004 FIX: Clear errno before read loop — stale errno from
	// prior system calls could cause false disconnect detection below.
	errno = 0;

	struct input_event ev;
	int accumDx = 0, accumDy = 0;
	int buttonChanges = 0;
	int buttonState = _prevMouseButtons;
	InputSynDrop sd = { false, false };

	while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (input_syn_drop_skip(&sd, &ev)) continue;
		if (ev.type == EV_REL) {
			if (ev.code == REL_X) accumDx += ev.value;
			else if (ev.code == REL_Y) accumDy += ev.value;
		} else if (ev.type == EV_KEY) {
			int bit = -1;
			if (ev.code == BTN_LEFT)   bit = 0;
			if (ev.code == BTN_RIGHT)  bit = 1;
			if (ev.code == BTN_MIDDLE) bit = 2;
			if (bit >= 0) {
				if (ev.value) buttonState |=  (1 << bit);
				else          buttonState &= ~(1 << bit);
				buttonChanges |= (1 << bit);
			}
		}
	}

	if (errno == ENODEV || errno == EBADF) {
		debug("Arca: mouse disconnected (event%d)", _mouseNodes[slot]);
		close(fd);
		fd = -1;
		_mouseNodes[slot] = -1;
		return false;
	}

	// SYN_DROPPED (input_scan.h): a release may have been discarded with the
	// overflow, so take the button levels from the kernel and let the loop
	// below emit whichever edges differ.
	unsigned long keys[INPUT_SCAN_NLONGS(KEY_MAX + 1)];
	if (sd.resync) {
		memset(keys, 0, sizeof(keys));
		int lvl = 0;
		if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) >= 0)
			lvl = (input_caps_test(keys, BTN_LEFT)   ? 1 : 0) |
			      (input_caps_test(keys, BTN_RIGHT)  ? 2 : 0) |
			      (input_caps_test(keys, BTN_MIDDLE) ? 4 : 0);
		buttonChanges |= (buttonState ^ lvl) | (_prevMouseButtons ^ lvl);
		buttonState = lvl;
	}

	// BUG-INPUT-004 FIX: Helper to flush accumulated mouse movement into the
	// pending event queue before returning a button event.  Without this,
	// movement accumulated during the same read batch as a button change would
	// be lost because the local accumDx/accumDy variables are discarded on return.
	auto flushMouseMovement = [&]() {
		if (accumDx == 0 && accumDy == 0) return;
		float speed = sqrtf((float)(accumDx * accumDx + accumDy * accumDy));
		float multiplier;
		if (speed < (float)_mouseLowThreshold)
			multiplier = 1.0f;
		else if (speed < (float)_mouseHighThreshold)
			multiplier = _mouseSensitivity;
		else
			multiplier = _mouseSensitivity * _mouseAcceleration;
		int mx = _mouseX + (int)(accumDx * multiplier);
		int my = _mouseY + (int)(accumDy * multiplier);
		// Clamp to wherever the current content is — the touch-safe rectangle in
		// the GUI, the picture in game mode.  A cursor parked outside the content
		// is a trap, not extra range.
		int sx, sy, sw, sh;
		cursorBounds(sx, sy, sw, sh);
		if (mx < sx) mx = sx;
		if (mx >= sx + sw) mx = sx + sw - 1;
		if (my < sy) my = sy;
		if (my >= sy + sh) my = sy + sh - 1;
		_mouseX = mx;
		_mouseY = my;
		int gmx, gmy;
		transformCoordinates(_mouseX, _mouseY, gmx, gmy);
		Common::Event moveEv;
		moveEv.type = Common::EVENT_MOUSEMOVE;
		moveEv.mouse.x = gmx;
		moveEv.mouse.y = gmy;
		pushEvent(moveEv);
		accumDx = 0;
		accumDy = 0;
	};

	// Button changes have priority
	for (int bit = 0; bit < 3; bit++) {
		if (!(buttonChanges & (1 << bit))) continue;

		bool nowDown = (buttonState & (1 << bit)) != 0;
		bool wasDown = (_prevMouseButtons & (1 << bit)) != 0;
		if (nowDown == wasDown) continue;

		if (bit == 2) {
			// Middle click -> Ctrl+F5 (GMM)
			if (nowDown) {
				Common::Event ctrlDown, f5Down, f5Up, ctrlUp;
				ctrlDown.type        = Common::EVENT_KEYDOWN;
				ctrlDown.kbd.keycode = Common::KEYCODE_LCTRL;
				ctrlDown.kbd.flags   = Common::KBD_CTRL;
				ctrlDown.kbd.ascii   = 0;
				f5Down.type          = Common::EVENT_KEYDOWN;
				f5Down.kbd.keycode   = Common::KEYCODE_F5;
				f5Down.kbd.flags     = Common::KBD_CTRL;
				f5Down.kbd.ascii     = 0;
				f5Up = f5Down;
				f5Up.type            = Common::EVENT_KEYUP;
				ctrlUp = ctrlDown;
				ctrlUp.type          = Common::EVENT_KEYUP;
				ctrlUp.kbd.flags     = 0;
				pushEvent(ctrlDown);
				pushEvent(f5Down);
				pushEvent(f5Up);
				pushEvent(ctrlUp);
			}
			// BUG-INPUT-004 FIX: Only update bit 2 so other button changes
			// in the same batch are not masked.
			if (nowDown) _prevMouseButtons |= (1 << 2);
			else         _prevMouseButtons &= ~(1 << 2);
			continue;
		}

		int gameX, gameY;
		transformCoordinates(_mouseX, _mouseY, gameX, gameY);

		if (bit == 0)
			event.type = nowDown ? Common::EVENT_LBUTTONDOWN : Common::EVENT_LBUTTONUP;
		else
			event.type = nowDown ? Common::EVENT_RBUTTONDOWN : Common::EVENT_RBUTTONUP;

		event.mouse.x = gameX;
		event.mouse.y = gameY;
		// BUG-INPUT-004 FIX: Only update the specific bit so other button
		// changes in the same batch are not masked.
		if (nowDown) _prevMouseButtons |= (1 << bit);
		else         _prevMouseButtons &= ~(1 << bit);
		// BUG-INPUT-004 FIX: Flush any accumulated movement before returning
		// the button event, so movement is not lost.
		flushMouseMovement();
		return true;
	}
	_prevMouseButtons = buttonState;

	// Movement with 3-tier acceleration (same model as gamepad.c)
	if (accumDx != 0 || accumDy != 0) {
		float speed = sqrtf((float)(accumDx * accumDx + accumDy * accumDy));
		float multiplier;

		if (speed < (float)_mouseLowThreshold)
			multiplier = 1.0f;
		else if (speed < (float)_mouseHighThreshold)
			multiplier = _mouseSensitivity;
		else
			multiplier = _mouseSensitivity * _mouseAcceleration;

		_mouseX += (int)(accumDx * multiplier);
		_mouseY += (int)(accumDy * multiplier);

		// Clamp to wherever the current content is (see cursorBounds()).
		{
			int sx, sy, sw, sh;
			cursorBounds(sx, sy, sw, sh);
			if (_mouseX < sx) _mouseX = sx;
			if (_mouseY < sy) _mouseY = sy;
			if (_mouseX >= sx + sw) _mouseX = sx + sw - 1;
			if (_mouseY >= sy + sh) _mouseY = sy + sh - 1;
		}

		int gameX, gameY;
		transformCoordinates(_mouseX, _mouseY, gameX, gameY);
		event.type = Common::EVENT_MOUSEMOVE;
		event.mouse.x = gameX;
		event.mouse.y = gameY;
		return true;
	}

	return false;
}

// =========================================================================
// Gamepad polling
// =========================================================================

bool ArcaEventSource::pollGamepad(Common::Event &event) {
	if (_gamepadFd < 0) return false;

	// BUG-INPUT-004 FIX: Clear errno before read loop — stale errno from
	// prior system calls could cause false disconnect detection below.
	errno = 0;

	struct input_event ev;
	int buttonChanges = 0;
	int buttonState = _prevGamepadButtons;
	// Bit assignments: 0=South(A), 1=East(B), 2=West(X), 3=North(Y),
	//                  4=Start, 5=Select, 6=TL, 7=TR

	InputSynDrop sd = { false, false };

	// The edge loop below returns on the first change, so with several buttons
	// to release the rest would be lost with this call's locals.  While a
	// resync is unfinished the next call compares every bit against the
	// remembered kernel levels again.
	static bool resyncPending = false;
	static int resyncLevels = 0;
	if (resyncPending) {
		buttonState = resyncLevels;
		buttonChanges = 0xff;
	}

	// Kernel key code -> button bit above, or -1.  The map holds native (xpad)
	// codes; a hid-generic pad's are not.
	auto padBit = [&](int kcode) -> int {
		int code = input_pad_key((InputPadLayout)_gamepadLayout, kcode);
		if (code == _gamepadMap.btnSouth)  return 0;
		if (code == _gamepadMap.btnEast)   return 1;
		if (code == _gamepadMap.btnWest)   return 2;
		if (code == _gamepadMap.btnNorth)  return 3;
		if (code == _gamepadMap.btnStart)  return 4;
		if (code == _gamepadMap.btnSelect) return 5;
		if (code == _gamepadMap.btnTL)     return 6;
		if (code == _gamepadMap.btnTR)     return 7;
		return -1;
	};

	while (read(_gamepadFd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (input_syn_drop_skip(&sd, &ev)) continue;
		if (ev.type == EV_ABS) {
			if (ev.code == _gamepadMap.stickXAxis)      _gamepadAxisX = ev.value;
			else if (ev.code == _gamepadMap.stickYAxis)  _gamepadAxisY = ev.value;
			else if (ev.code == _gamepadMap.hatXAxis)    _gamepadHatX  = ev.value;
			else if (ev.code == _gamepadMap.hatYAxis)    _gamepadHatY  = ev.value;
		} else if (ev.type == EV_KEY) {
			int bit = padBit(ev.code);

			if (bit >= 0) {
				if (ev.value) buttonState |=  (1 << bit);
				else          buttonState &= ~(1 << bit);
				buttonChanges |= (1 << bit);
			}
		}
	}

	if (errno == ENODEV || errno == EBADF) {
		debug("Arca: gamepad disconnected");
		close(_gamepadFd);
		_gamepadFd = -1;
		_gamepadLayout = INPUT_PAD_NATIVE;
		return false;
	}

	// SYN_DROPPED (input_scan.h): a release, or an axis returning to centre,
	// may have been discarded with the overflow.  Take the button levels from
	// the kernel and let the loop below emit whichever edges differ; refresh
	// the sticks and hat from EVIOCGABS so a lost re-centre cannot leave the
	// cursor drifting.
	if (sd.resync) {
		unsigned long keys[INPUT_SCAN_NLONGS(KEY_MAX + 1)];
		memset(keys, 0, sizeof(keys));
		if (ioctl(_gamepadFd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
			int lvl = 0;
			for (int c = BTN_MISC; c <= KEY_MAX; c++) {
				if (!input_caps_test(keys, c)) continue;
				int b = padBit(c);
				if (b >= 0) lvl |= (1 << b);
			}
			buttonChanges |= (buttonState ^ lvl) | (_prevGamepadButtons ^ lvl);
			buttonState = lvl;
			resyncPending = true;
			resyncLevels = lvl;
		}
		struct input_absinfo info;
		if (_gamepadMap.stickXAxis >= 0 && ioctl(_gamepadFd, EVIOCGABS(_gamepadMap.stickXAxis), &info) == 0)
			_gamepadAxisX = info.value;
		if (_gamepadMap.stickYAxis >= 0 && ioctl(_gamepadFd, EVIOCGABS(_gamepadMap.stickYAxis), &info) == 0)
			_gamepadAxisY = info.value;
		if (_gamepadMap.hatXAxis >= 0 && ioctl(_gamepadFd, EVIOCGABS(_gamepadMap.hatXAxis), &info) == 0)
			_gamepadHatX = info.value;
		if (_gamepadMap.hatYAxis >= 0 && ioctl(_gamepadFd, EVIOCGABS(_gamepadMap.hatYAxis), &info) == 0)
			_gamepadHatY = info.value;
	}

	// Process button changes
	for (int bit = 0; bit < 8; bit++) {
		if (!(buttonChanges & (1 << bit))) continue;
		bool nowDown = (buttonState & (1 << bit)) != 0;
		bool wasDown = (_prevGamepadButtons & (1 << bit)) != 0;
		if (nowDown == wasDown) continue;

		// BUG-INPUT-004 FIX: Only update the specific bit so other button
		// changes in the same batch are not masked.
		if (nowDown) _prevGamepadButtons |= (1 << bit);
		else         _prevGamepadButtons &= ~(1 << bit);

		switch (bit) {
		case 0: { // South (A) -> left click
			int gx, gy;
			transformCoordinates(_mouseX, _mouseY, gx, gy);
			event.type = nowDown ? Common::EVENT_LBUTTONDOWN : Common::EVENT_LBUTTONUP;
			event.mouse.x = gx;
			event.mouse.y = gy;
			return true;
		}
		case 1: { // East (B) -> right click
			int gx, gy;
			transformCoordinates(_mouseX, _mouseY, gx, gy);
			event.type = nowDown ? Common::EVENT_RBUTTONDOWN : Common::EVENT_RBUTTONUP;
			event.mouse.x = gx;
			event.mouse.y = gy;
			return true;
		}
		case 2: // West (X) -> Space (skip cutscene)
			event.type = nowDown ? Common::EVENT_KEYDOWN : Common::EVENT_KEYUP;
			event.kbdRepeat = false;
			event.kbd.keycode = Common::KEYCODE_SPACE;
			event.kbd.ascii = ' ';
			event.kbd.flags = 0;
			return true;
		case 3: // North (Y) -> Escape (skip dialog)
			event.type = nowDown ? Common::EVENT_KEYDOWN : Common::EVENT_KEYUP;
			event.kbdRepeat = false;
			event.kbd.keycode = Common::KEYCODE_ESCAPE;
			event.kbd.ascii = 27;
			event.kbd.flags = 0;
			return true;
		case 4: // Start -> Ctrl+F5 (GMM)
			if (nowDown) {
				Common::Event ctrlDown, f5Down, f5Up, ctrlUp;
				ctrlDown.type        = Common::EVENT_KEYDOWN;
				ctrlDown.kbd.keycode = Common::KEYCODE_LCTRL;
				ctrlDown.kbd.flags   = Common::KBD_CTRL;
				ctrlDown.kbd.ascii   = 0;
				f5Down.type          = Common::EVENT_KEYDOWN;
				f5Down.kbd.keycode   = Common::KEYCODE_F5;
				f5Down.kbd.flags     = Common::KBD_CTRL;
				f5Down.kbd.ascii     = 0;
				f5Up = f5Down;
				f5Up.type            = Common::EVENT_KEYUP;
				ctrlUp = ctrlDown;
				ctrlUp.type          = Common::EVENT_KEYUP;
				ctrlUp.kbd.flags     = 0;
				pushEvent(f5Down);
				pushEvent(f5Up);
				pushEvent(ctrlUp);
				event = ctrlDown;
				return true;
			}
			break;
		case 5: // Select -> F5 (save/load)
			event.type = nowDown ? Common::EVENT_KEYDOWN : Common::EVENT_KEYUP;
			event.kbdRepeat = false;
			event.kbd.keycode = Common::KEYCODE_F5;
			event.kbd.ascii = Common::ASCII_F5;
			event.kbd.flags = 0;
			return true;
		case 6: // Left bumper -> Period (skip text)
			event.type = nowDown ? Common::EVENT_KEYDOWN : Common::EVENT_KEYUP;
			event.kbdRepeat = false;
			event.kbd.keycode = Common::KEYCODE_PERIOD;
			event.kbd.ascii = '.';
			event.kbd.flags = 0;
			return true;
		case 7: // Right bumper -> Escape
			event.type = nowDown ? Common::EVENT_KEYDOWN : Common::EVENT_KEYUP;
			event.kbdRepeat = false;
			event.kbd.keycode = Common::KEYCODE_ESCAPE;
			event.kbd.ascii = 27;
			event.kbd.flags = 0;
			return true;
		}
	}
	_prevGamepadButtons = buttonState;
	resyncPending = false;

	// Cursor movement from analog stick + D-pad
	uint32 now = 0;
	if (g_system) now = g_system->getMillis();
	if (now - _lastCursorMove < GAMEPAD_CURSOR_INTERVAL)
		return false;

	int moveX = 0, moveY = 0;

	// Analog stick
	{
		int halfRange = (_gamepadAxisMax - _gamepadAxisMin) / 2;
		if (halfRange > 0) {
			int offX = _gamepadAxisX - _gamepadAxisCenter;
			int offY = _gamepadAxisY - _gamepadAxisCenter;
			int dz = (halfRange * _gamepadDeadzonePct) / 100;

			if (offX > -dz && offX < dz) offX = 0;
			if (offY > -dz && offY < dz) offY = 0;

			int effRange = halfRange - dz;
			if (effRange > 0) {
				if (offX > 0) offX -= dz; else if (offX < 0) offX += dz;
				if (offY > 0) offY -= dz; else if (offY < 0) offY += dz;
				moveX = (offX * GAMEPAD_CURSOR_SPEED) / effRange;
				moveY = (offY * GAMEPAD_CURSOR_SPEED) / effRange;
			}
		}
	}

	// D-pad
	if (_gamepadHatX < 0) moveX -= GAMEPAD_CURSOR_SPEED;
	if (_gamepadHatX > 0) moveX += GAMEPAD_CURSOR_SPEED;
	if (_gamepadHatY < 0) moveY -= GAMEPAD_CURSOR_SPEED;
	if (_gamepadHatY > 0) moveY += GAMEPAD_CURSOR_SPEED;

	// Clamp
	if (moveX < -GAMEPAD_CURSOR_SPEED) moveX = -GAMEPAD_CURSOR_SPEED;
	if (moveX >  GAMEPAD_CURSOR_SPEED) moveX =  GAMEPAD_CURSOR_SPEED;
	if (moveY < -GAMEPAD_CURSOR_SPEED) moveY = -GAMEPAD_CURSOR_SPEED;
	if (moveY >  GAMEPAD_CURSOR_SPEED) moveY =  GAMEPAD_CURSOR_SPEED;

	if (moveX != 0 || moveY != 0) {
		_lastCursorMove = now;
		_mouseX += moveX;
		_mouseY += moveY;

		// Clamp to wherever the current content is (see cursorBounds()).
		{
			int sx, sy, sw, sh;
			cursorBounds(sx, sy, sw, sh);
			if (_mouseX < sx) _mouseX = sx;
			if (_mouseY < sy) _mouseY = sy;
			if (_mouseX >= sx + sw) _mouseX = sx + sw - 1;
			if (_mouseY >= sy + sh) _mouseY = sy + sh - 1;
		}

		int gx, gy;
		transformCoordinates(_mouseX, _mouseY, gx, gy);
		event.type = Common::EVENT_MOUSEMOVE;
		event.mouse.x = gx;
		event.mouse.y = gy;
		return true;
	}

	return false;
}

// =========================================================================
// Touch polling — extracted from original pollEvent() without behavior changes
// =========================================================================

bool ArcaEventSource::pollTouch(Common::Event &event) {
	if (!_touchInitialized || !_touchInput)
		return false;

	// Poll touch input (non-blocking)
	touch_poll(_touchInput);
	TouchState state = touch_get_state(_touchInput);

	// After gesture or context switch, wait for finger lift
	if (_waitForRelease) {
		if (state.released || (!state.pressed && !state.held)) {
			_waitForRelease = false;
			_touchPhase = TOUCH_NONE;
			debug("Arca: touch lockout cleared, input re-enabled");
		}
		return false;
	}

	// Detect overlay visibility transition
	{
		OSystem_Arca *sysChk = arcaSystem();
		bool overlayNow = (sysChk && sysChk->getGraphicsManager() &&
			((ArcaGraphicsManager *)sysChk->getGraphicsManager())->isOverlayVisible());
		if (overlayNow != _prevOverlayVisible) {
			_prevOverlayVisible = overlayNow;
			debug("Arca: overlay transition -> %s", overlayNow ? "shown" : "hidden");
			if (_touchPhase == TOUCH_HELD) {
				_touchPhase = TOUCH_NONE;
				_waitForRelease = true;
				int gameX, gameY;
				transformCoordinates(_lastTouchX, _lastTouchY, gameX, gameY);
				event.type = Common::EVENT_LBUTTONUP;
				event.mouse.x = gameX;
				event.mouse.y = gameY;
				debug("Arca: overlay transition LBUTTONUP injected at (%d,%d)", gameX, gameY);
				return true;
			} else if (_touchPhase == TOUCH_PRESSED) {
				_touchPhase = TOUCH_NONE;
				_waitForRelease = true;
			}
		}
	}

	uint32 currentTime = g_system->getMillis();

	// Debug logging
	if (arcaDebugMode()) {
		static bool lastPressed = false;
		static bool lastHeld = false;
		static bool lastReleased = false;
		if (state.pressed != lastPressed || state.held != lastHeld || state.released != lastReleased) {
			debug("Touch state: pressed=%d held=%d released=%d phase=%d",
			      state.pressed, state.held, state.released, _touchPhase);
			lastPressed = state.pressed;
			lastHeld = state.held;
			lastReleased = state.released;
		}
	}

	switch (_touchPhase) {
		case TOUCH_NONE:
			if (state.pressed || state.held) {
				_lastTouchX = state.x;
				_lastTouchY = state.y;
				_touchStartTime = currentTime;
				_touchPhase = TOUCH_PRESSED;
				_buttonDownSent = false;
				_longPressFired = false;

				checkGestures(state.x, state.y, currentTime);
				if (_pendingCount > 0) {
					event = _pending[_pendingHead];
					_pendingHead = (_pendingHead + 1) % MAX_PENDING;
					_pendingCount--;
					_touchPhase = TOUCH_NONE;
					return true;
				}

				if (cornerFor(state.x, state.y) != CORNER_COUNT) {
					_waitForRelease = true;
					_touchPhase = TOUCH_NONE;
					return false;
				}

				if (arcaDebugMode()) {
					debug("TOUCH_NONE -> TOUCH_PRESSED: sending MOUSEMOVE at (%d,%d)", state.x, state.y);
					OSystem_Arca *system = arcaSystem();
					if (system && system->getGraphicsManager())
						((ArcaGraphicsManager *)system->getGraphicsManager())->addTouchPoint(state.x, state.y);
				}

				int gameX, gameY;
				transformCoordinates(state.x, state.y, gameX, gameY);
				event.type = Common::EVENT_MOUSEMOVE;
				event.mouse.x = gameX;
				event.mouse.y = gameY;
				return true;
			}
			break;

		case TOUCH_PRESSED:
			if (!_buttonDownSent && state.held) {
				_buttonDownSent = true;
				_touchPhase = TOUCH_HELD;

				int gameX, gameY;
				transformCoordinates(_lastTouchX, _lastTouchY, gameX, gameY);
				debug("TOUCH_PRESSED -> TOUCH_HELD: sending LBUTTONDOWN at (%d,%d)", gameX, gameY);
				event.type = Common::EVENT_LBUTTONDOWN;
				event.mouse.x = gameX;
				event.mouse.y = gameY;
				return true;
			}
			if (!_buttonDownSent && (state.released || !state.held)) {
				int gameX, gameY;
				transformCoordinates(_lastTouchX, _lastTouchY, gameX, gameY);
				debug("TOUCH_PRESSED: quick tap at (%d,%d) - sending LBUTTONDOWN+UP", gameX, gameY);

				Common::Event upEvent;
				upEvent.type = Common::EVENT_LBUTTONUP;
				upEvent.mouse.x = gameX;
				upEvent.mouse.y = gameY;
				pushEvent(upEvent);

				_buttonDownSent = true;
				_touchPhase = TOUCH_NONE;
				event.type = Common::EVENT_LBUTTONDOWN;
				event.mouse.x = gameX;
				event.mouse.y = gameY;
				return true;
			}
			break;

		case TOUCH_HELD:
			if (state.released || !state.held) {
				_touchPhase = TOUCH_NONE;
				int gameX, gameY;
				transformCoordinates(_lastTouchX, _lastTouchY, gameX, gameY);

				if (_longPressFired) {
					debug("TOUCH_HELD -> TOUCH_NONE: sending RBUTTONUP (long-press release)");
					event.type = Common::EVENT_RBUTTONUP;
				} else {
					debug("TOUCH_HELD -> TOUCH_NONE: sending LBUTTONUP (duration=%dms)",
					      currentTime - _touchStartTime);
					event.type = Common::EVENT_LBUTTONUP;
				}

				event.mouse.x = gameX;
				event.mouse.y = gameY;
				return true;
			}

			if (!_longPressFired && (currentTime - _touchStartTime) >= LONG_PRESS_TIME) {
				_longPressFired = true;
				int gameX, gameY;
				transformCoordinates(_lastTouchX, _lastTouchY, gameX, gameY);
				debug("TOUCH_HELD: long-press at (%d,%d) - LBUTTONUP + queue RBUTTONDOWN", gameX, gameY);

				Common::Event rbDown;
				rbDown.type = Common::EVENT_RBUTTONDOWN;
				rbDown.mouse.x = gameX;
				rbDown.mouse.y = gameY;
				pushEvent(rbDown);

				event.type = Common::EVENT_LBUTTONUP;
				event.mouse.x = gameX;
				event.mouse.y = gameY;
				return true;
			}

			if (state.held && (state.x != _lastTouchX || state.y != _lastTouchY)) {
				_lastTouchX = state.x;
				_lastTouchY = state.y;
				int gameX, gameY;
				transformCoordinates(state.x, state.y, gameX, gameY);
				event.type = Common::EVENT_MOUSEMOVE;
				event.mouse.x = gameX;
				event.mouse.y = gameY;
				return true;
			}
			break;
	}

	return false;
}

// =========================================================================
// Main pollEvent — orchestrates all input sources
// =========================================================================

bool ArcaEventSource::pollEvent(Common::Event &event) {
	// Pump timer callbacks (OPL sequencer, iMUSE, etc.) on every event poll.
	// checkTimers is gated to fire at most every 10 ms, so calling it here
	// (potentially hundreds of times/sec) is safe and low-overhead.
	if (g_system) {
		DefaultTimerManager *tm =
		    static_cast<DefaultTimerManager *>(g_system->getTimerManager());
		if (tm) tm->checkTimers(10);
	}

	// Drain any synthetic events queued by gesture detection or multi-event input
	if (_pendingCount > 0) {
		// While draining, keep polling touch hardware so we can detect release
		if (_touchInitialized && _touchInput)
			touch_poll(_touchInput);
		event = _pending[_pendingHead];
		_pendingHead = (_pendingHead + 1) % MAX_PENDING;
		_pendingCount--;
		return true;
	}

	// Hot-plug rescan, gated on the /dev/input fingerprint (input_scan.h):
	// every INPUT_SIG_CHECK_MS the directory is listed, opening no device, and
	// the incremental scan runs only if it changed.  Only while a slot is free —
	// a second keyboard or mouse can arrive while the first is held; held
	// nodes are skipped.  The slot test comes FIRST so a full house does not
	// consume a change: once a slot frees, the next check still compares
	// against the last scan's baseline and picks up a node plugged meanwhile.
	if (g_system &&
	    (countOpen(_keyboardFds, MAX_KEYBOARDS) < MAX_KEYBOARDS ||
	     countOpen(_mouseFds, MAX_MICE) < MAX_MICE || _gamepadFd < 0) &&
	    input_sig_gate_poll(&_nodeGate, g_system->getMillis()))
		scanInputDevices();

	// Poll all input sources — return first event found
	if (pollKeyboard(event)) return true;
	if (pollMouse(event))    return true;
	if (pollGamepad(event))  return true;

	// Existing touch handling (moved to pollTouch without behavior changes)
	return pollTouch(event);
}
