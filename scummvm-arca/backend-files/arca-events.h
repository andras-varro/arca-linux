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

#ifndef BACKENDS_EVENTS_ARCA_H
#define BACKENDS_EVENTS_ARCA_H

#include "common/events.h"

// Include C headers directly
extern "C" {
#include "touch_input.h"
#include "framebuffer.h"   // screen_base_width/height (the visible screen size)
#include "input_scan.h"    // InputSigGate (hot-plug check), InputConfig
}

class ArcaEventSource : public Common::EventSource {
public:
	ArcaEventSource();
	virtual ~ArcaEventSource();

	bool pollEvent(Common::Event &event) override;
	
	/**
	 * Allow keymapper processing for events from this source.
	 * ScummVM engines and GUI rely on the keymapper to convert keyboard
	 * and gamepad events into actions.  Touch events (MOUSEMOVE,
	 * LBUTTONDOWN/UP) are not keymapped anyway, so returning true is safe.
	 */
	bool allowMapping() const override { return true; }

	// Set game screen dimensions for coordinate transformation
	void setGameScreenSize(int width, int height, int offsetX, int offsetY);

	// Pick up the framebuffer's logical (visible) size and bezel viewport.
	// Call once the framebuffer is up: touch is initialised before it, so the
	// geometry it read at construction time is the pre-bezel default.
	void syncScreenGeometry();

private:
	// Logical (visible) screen size — the framebuffer minus the bezel
	int _screenW;
	int _screenH;

	// -------------------------------------------------------
	// Touch input (existing)
	// -------------------------------------------------------
	TouchInput *_touchInput;
	bool _touchInitialized;

	// Touch state machine
	enum TouchPhase {
		TOUCH_NONE,
		TOUCH_PRESSED,    // Just pressed, need to send MOUSEMOVE + LBUTTONDOWN
		TOUCH_HELD        // Held down, send MOUSEMOVE on position change or LBUTTONUP on release
	};
	
	TouchPhase _touchPhase;
	bool _buttonDownSent;
	bool _longPressFired;    // true after 500ms LBUTTONUP+RBUTTONDOWN sent
	bool _waitForRelease;    // set after gesture or context switch; blocks input until finger lifts
	bool _prevOverlayVisible; // tracks overlay state to detect GMM open/close transitions
	int _lastTouchX;
	int _lastTouchY;
	uint32 _touchStartTime;

	// Game screen transformation
	int _gameWidth;
	int _gameHeight;

	// Long press for right-click
	static const uint32 LONG_PRESS_TIME = 500; // milliseconds

	// -------------------------------------------------------
	// Gesture detection
	// -------------------------------------------------------
	// Corner zones (on 800x480 screen):
	//   Bottom-left  (x<80, y>400) triple-tap  → Virtual Keyboard
	//   Bottom-right (x>720, y>400) triple-tap → Global Main Menu (Ctrl+F5)
	enum Corner { CORNER_BL = 0, CORNER_BR = 1, CORNER_COUNT = 2 };
	struct CornerTaps {
		uint32 timestamps[3]; // ring buffer of last 3 tap times
		int    count;         // how many taps accumulated
	};
	CornerTaps _cornerTaps[CORNER_COUNT];

	// Pending synthetic events queued by gesture detection and multi-event input
	static const int MAX_PENDING = 16;
	Common::Event _pending[MAX_PENDING];
	int           _pendingHead;
	int           _pendingCount;

	void   checkGestures(int touchX, int touchY, uint32 now);
	Corner cornerFor(int x, int y) const; // returns CORNER_COUNT if not in any corner
	// The touch-safe rectangle (visible AND reachable), read from the graphics
	// manager so it cannot disagree with what is drawn. Everything hit-tested
	// here — gesture corners, overlay coordinates, the virtual cursor — is
	// confined to it; the whole logical screen before the manager exists.
	void   safeRect(int &x, int &y, int &w, int &h) const;
	// Where the virtual cursor may go: the safe rect in the GUI, the game
	// picture in game mode (so the arca_content_area=visible opt-out stays
	// mouse-reachable even outside the safe rect).
	void   cursorBounds(int &x, int &y, int &w, int &h) const;
	void   pushEvent(const Common::Event &e);

	// Touch helper methods
	void initTouch();
	void closeTouch();
	bool pollTouch(Common::Event &event);

	// Coordinate transformation (shared by touch, mouse, gamepad)
	void transformCoordinates(int touchX, int touchY, int &gameX, int &gameY);

	// -------------------------------------------------------
	// USB input device scanning and management
	// -------------------------------------------------------
	// Every keyboard node and every mouse node is held at once: a touchpad
	// keyboard exposes its own pointer node, and a second USB receiver brings
	// another, and both must drive the one cursor.  Unused slots are -1.
	// *Nodes[] records the N of /dev/input/eventN behind each fd so that a
	// rescan never opens a node that is already held.
	static const int MAX_KEYBOARDS = 8;
	static const int MAX_MICE = 8;
	int _keyboardFds[MAX_KEYBOARDS];
	int _keyboardNodes[MAX_KEYBOARDS];
	int _mouseFds[MAX_MICE];
	int _mouseNodes[MAX_MICE];
	int _mouseNext;  // round-robin start, so one busy mouse cannot starve another
	int _gamepadFd;
	int _gamepadLayout;  // InputPadLayout (input_scan.h) of _gamepadFd; 0 is native

	void scanInputDevices();         // common/input_scan.c walks /dev/input/event0..31
	void closeInputDevices();        // Close all USB device fds
	static int countOpen(const int *fds, int n);
	static bool addToSlot(int *fds, int *nodes, int n, int fd, int node);

	// Hot-plug check
	InputSigGate _nodeGate;          // input_scan.h: /dev/input fingerprint, checked every INPUT_SIG_CHECK_MS

	// -------------------------------------------------------
	// USB Keyboard support
	// -------------------------------------------------------
	bool pollKeyboard(Common::Event &event);
	bool pollKeyboardFd(int slot, Common::Event &event);

	// Keyboard modifier state tracking
	byte _modifierFlags;  // Current modifier state (KBD_SHIFT, KBD_CTRL, KBD_ALT)

	// Map Linux KEY_* scancode to ScummVM keycode + ascii
	struct KeyMapping {
		Common::KeyCode keycode;
		uint16 ascii;           // unshifted ascii value (0 if not printable)
		uint16 shiftAscii;      // shifted ascii value (0 if same or not printable)
	};
	static KeyMapping mapLinuxKey(int linuxKeyCode, byte modifiers);

	// -------------------------------------------------------
	// USB Mouse support
	// -------------------------------------------------------
	bool pollMouse(Common::Event &event);
	bool pollMouseFd(int slot, Common::Event &event);

	// Mouse state
	int _mouseX, _mouseY;           // Current cursor position in screen coords (0..799, 0..479)
	int _prevMouseButtons;           // Previous button state for edge detection

	// Mouse acceleration config (loaded from /etc/input_config.conf)
	float _mouseSensitivity;         // default: input_config_defaults()
	float _mouseAcceleration;        // default: input_config_defaults()
	int   _mouseLowThreshold;        // default: input_config_defaults()
	int   _mouseHighThreshold;       // default: input_config_defaults()

	// -------------------------------------------------------
	// USB Gamepad support
	// -------------------------------------------------------
	bool pollGamepad(Common::Event &event);

	// Gamepad state
	int _gamepadAxisX, _gamepadAxisY;       // Current analog stick values (raw)
	int _gamepadHatX, _gamepadHatY;         // Current D-pad hat values (-1, 0, +1)
	int _gamepadAxisMin, _gamepadAxisMax;    // Axis range from EVIOCGABS
	int _gamepadAxisCenter;                  // Center calibration
	int _prevGamepadButtons;                 // Previous button state for edge detection
	uint32 _lastCursorMove;                  // For cursor movement rate limiting
	int _gamepadDeadzonePct;                 // gamepad_deadzone (input_config.conf), percent of half-range

	// Gamepad cursor movement constants
	static const int GAMEPAD_CURSOR_SPEED = 5;    // Max pixels per poll at full deflection
	static const uint32 GAMEPAD_CURSOR_INTERVAL = 16; // ~60 Hz cursor movement

	// Gamepad button mapping (configurable for clone controllers)
	struct GamepadBtnMap {
		int btnSouth;    // A — left click (default BTN_SOUTH / 304)
		int btnEast;     // B — right click (default BTN_EAST / 305)
		int btnWest;     // X — space/skip cutscene (default BTN_WEST / 308)
		int btnNorth;    // Y — escape/skip dialog (default BTN_NORTH / 307)
		int btnStart;    // Start — Ctrl+F5 GMM (default BTN_START / 315)
		int btnSelect;   // Select — F5 save/load (default BTN_SELECT / 314)
		int btnTL;       // Left bumper — period/skip text (default BTN_TL / 310)
		int btnTR;       // Right bumper — escape (default BTN_TR / 311)
		int hatXAxis;    // D-pad X axis (default ABS_HAT0X / 16)
		int hatYAxis;    // D-pad Y axis (default ABS_HAT0Y / 17)
		int stickXAxis;  // Left stick X (default ABS_X / 0)
		int stickYAxis;  // Left stick Y (default ABS_Y / 1)
	};
	GamepadBtnMap _gamepadMap;

	void loadGamepadAxisCalibration();

	// -------------------------------------------------------
	// Config file loading
	// -------------------------------------------------------
	void loadInputConfig();
};

#endif
