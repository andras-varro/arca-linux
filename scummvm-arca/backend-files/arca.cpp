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

// Define forbidden symbol exceptions BEFORE including any headers
#define FORBIDDEN_SYMBOL_EXCEPTION_FILE
#define FORBIDDEN_SYMBOL_EXCEPTION_stdout
#define FORBIDDEN_SYMBOL_EXCEPTION_stderr
#define FORBIDDEN_SYMBOL_EXCEPTION_fputs
#define FORBIDDEN_SYMBOL_EXCEPTION_exit
#define FORBIDDEN_SYMBOL_EXCEPTION_time_h
#define FORBIDDEN_SYMBOL_EXCEPTION_unistd_h
#define FORBIDDEN_SYMBOL_EXCEPTION_write
#define FORBIDDEN_SYMBOL_EXCEPTION_getenv
#define FORBIDDEN_SYMBOL_EXCEPTION_fopen
#define FORBIDDEN_SYMBOL_EXCEPTION_fclose
#define FORBIDDEN_SYMBOL_EXCEPTION_fprintf
#define FORBIDDEN_SYMBOL_EXCEPTION_fflush
#define FORBIDDEN_SYMBOL_EXCEPTION_fseek
#define FORBIDDEN_SYMBOL_EXCEPTION_ftell
#define FORBIDDEN_SYMBOL_EXCEPTION_setvbuf
#define FORBIDDEN_SYMBOL_EXCEPTION_mkdir
#define FORBIDDEN_SYMBOL_EXCEPTION_rename

#include "backends/platform/arca/arca.h"
#include "backends/platform/arca/arca-graphics.h"
#include "backends/platform/arca/arca-events.h"
#include "backends/saves/default/default-saves.h"
#include "backends/timer/default/default-timer.h"
#include "backends/events/default/default-events.h"
#include "backends/mixer/oss/oss-mixer.h"
#include "backends/fs/posix/posix-fs-factory.h"
#include "common/archive.h"
#include "common/fs.h"
#include "base/main.h"
#include "common/scummsys.h"
#include "common/config-manager.h"
#ifdef ENABLE_VKEYBD
#include "backends/vkeybd/virtual-keyboard.h"
#endif

#include "backends/mutex/null/null-mutex.h"
#include <unistd.h>
#include <errno.h>

// Timer callbacks are pumped cooperatively from delayMillis() and pollEvent()
// via DefaultTimerManager::checkTimers(10).  No background thread is used —
// any real pthread + real mutex causes a deadlock with the SCHED_RR audio
// thread on this single-core ARM during ScummVM init.  NullMutexInternal is
// safe here because all timer callbacks run exclusively on the main thread.

#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>

// _logFile is void* in the header to avoid leaking <stdio.h>.
// Cast helper for use in this file.
#define LOG_FP  ((FILE *)_logFile)

// Debug mode: set ARCA_DEBUG=1 on the device to enable visual touch
// feedback and verbose touch-state logging.
bool arcaDebugMode() {
	static bool checked = false;
	static bool enabled = false;
	if (!checked) {
		checked = true;
		const char *v = getenv("ARCA_DEBUG");
		enabled = (v && v[0] != '\0' && v[0] != '0');
		if (enabled)
			debug("Arca: debug mode enabled (ARCA_DEBUG=%s)", v);
	}
	return enabled;
}

// Content area: by default the game picture is letterboxed into the TOUCH-SAFE
// rectangle (see arca-graphics.cpp), because we cannot audit a game for
// which of its pixels must be pressable — a verb bar or inventory on the bottom
// row has to be reachable.  Opt out with ARCA_CONTENT_AREA=visible for a
// one-off run, or arca_content_area=visible in scummvm.ini to persist it.
// The OVERLAY (launcher / GMM / virtual keyboard) is deliberately NOT affected:
// it is nothing but buttons, so it always stays inside the safe rect.
//
// initBackend() writes the key on first run, so the "safe" default is visible in
// scummvm.ini rather than being an undocumented reader.  The else-branch below is
// still needed: it covers the run that CREATES the key (this can be called before
// or after initBackend depending on the graphics path) and any hand-emptied file.
bool arcaFullContentArea() {
	static bool checked = false;
	static bool full = false;
	if (!checked) {
		checked = true;
		// Copy into a String — ConfMan.get() returns by value, so holding a
		// c_str() into it would dangle.
		Common::String mode;
		const char *env = getenv("ARCA_CONTENT_AREA");
		if (env && env[0] != '\0')
			mode = env;
		else if (ConfMan.hasKey("arca_content_area"))
			mode = ConfMan.get("arca_content_area");
		else
			mode = "safe";

		full = mode.equalsIgnoreCase("visible");
		if (full)
			debug("Arca: content area = visible (game picture may extend "
			      "outside the touch-safe rectangle)");
		else if (!mode.equalsIgnoreCase("safe"))
			warning("Arca: content area '%s' is not 'safe' or 'visible' — using 'safe'",
			        mode.c_str());
	}
	return full;
}

// Cached pointer — avoids dynamic_cast<OSystem_Arca*>(g_system) on every poll
static OSystem_Arca *s_arcaSystem = nullptr;
OSystem_Arca *arcaSystem() { return s_arcaSystem; }

OSystem_Arca::OSystem_Arca()
	: _eventSource(nullptr)
	, _logFile(nullptr)
	, _logBytes(0)
#ifdef ENABLE_VKEYBD
	, _vkbd(nullptr)
#endif
{
	s_arcaSystem = this;

	// Set up filesystem factory
	_fsFactory = new POSIXFilesystemFactory();

	// Initialize start time
	gettimeofday(&_startTime, 0);
}

OSystem_Arca::~OSystem_Arca() {
	if (_logFile) {
		fprintf(LOG_FP, "=== ScummVM shutting down (pid %d) ===\n", (int)getpid());
		fclose(LOG_FP);
		_logFile = nullptr;
	}
	delete _eventSource;
}

void OSystem_Arca::initBackend() {
	// Initialize file logger
	mkdir("/var/log/arca", 0755);
	_logFile = fopen("/var/log/arca/scummvm.log", "a");
	if (_logFile) {
		setvbuf(LOG_FP, nullptr, _IOLBF, 0);  // line-buffered for crash safety
		fseek(LOG_FP, 0, SEEK_END);
		_logBytes = ftell(LOG_FP);
		fprintf(LOG_FP, "=== ScummVM started (pid %d) ===\n", (int)getpid());
	}

	// Create graphics manager
	_graphicsManager = new ArcaGraphicsManager();
	
	// Create event source
	_eventSource = new ArcaEventSource();
	
	// Create event manager
	_eventManager = new DefaultEventManager(_eventSource);
	
	// Create timer manager — pumped cooperatively from delayMillis/pollEvent.
	_timerManager = new DefaultTimerManager();
	
	// Create save file manager
	// Saves go to /home/root/data/scummvm-saves, a plain directory on the root
	// partition p3 (a reflash wipes it; captured by
	// the ScummVM backup item).  The constructor argument is upstream's idiom: it does
	// ConfMan.registerDefault("savepath", ...), so an explicit savepath in
	// scummvm.ini still wins.  Without it saves landed in the process cwd
	// ("/" at boot, /home/root from SSH).
	static const char *const kSavePath = "/home/root/data/scummvm-saves";
	if (mkdir(kSavePath, 0755) != 0 && errno != EEXIST) {
		if (_logFile)
			fprintf(LOG_FP, "WARNING: cannot create savepath %s: errno %d\n", kSavePath, errno);
		warning("Arca: cannot create savepath %s (errno %d)", kSavePath, errno);
	}
	_savefileManager = new DefaultSaveFileManager(kSavePath);
	
	// Mixer over native_apps' audio_out — ALSA plughw:N,0 through libasound (TWL4030 or a USB DAC)
	_mixerManager = new OssMixerManager();
	_mixerManager->init();
	
	// Register /opt/games as extrapath so vkeybd_small.zip and other data
	// files are discoverable via loadKeyboardPack (which checks extrapath
	// before SearchMan). Also add to SearchMan as a fallback.
	if (!ConfMan.hasKey("extrapath"))
		ConfMan.set("extrapath", "/opt/games");
	{
		Common::FSNode dataDir("/opt/games");
		if (dataDir.isDirectory())
			SearchMan.addDirectory(dataDir.getPath(), dataDir);
	}

	// Register /opt/games as iconspath so ScummVM can scan for game icon
	// pack files (gui-icons*.dat).  generateZipSet() in gui/ThemeEngine
	// scans iconspath for matching .dat files.
	if (!ConfMan.hasKey("iconspath"))
		ConfMan.set("iconspath", "/opt/games");

	// Backend defaults written into the config file on first run.  ConfMan only
	// persists keys that were actually set — registerDefault() is not written
	// either — so a key we merely read with hasKey() never appears in
	// scummvm.ini.  Writing them makes the file the discovery surface, and the
	// !hasKey() guards mean a user's choice is never overwritten and a second
	// launch does not rewrite the file.
	//
	// Flushed explicitly rather than left for shutdown: quit() calls exit(0) and
	// bypasses ScummVM's normal flush, so a plain set() can be lost on the one
	// exit path this device actually takes.
	bool configDirty = false;

	// arca_content_area: see arcaFullContentArea() above.  ARCA_CONTENT_AREA
	// still takes precedence at read time and is deliberately NOT persisted —
	// it is documented as a one-off override.
	if (!ConfMan.hasKey("arca_content_area")) {
		ConfMan.set("arca_content_area", "safe");
		configDirty = true;
	}

	// Leaving a game must return to the ScummVM launcher, not terminate ScummVM.
	// base/main.cpp's launcher loop `break`s out — quitting the process — when a
	// game exits cleanly and neither this option nor kFeatureNoQuit is set; that
	// is upstream's default on every platform, and it is why exiting a game here
	// dropped the user all the way back to app_launcher.  Setting it also makes
	// DefaultEventManager rewrite EVENT_QUIT as EVENT_RETURN_TO_LAUNCHER while an
	// engine that supports it is running, so the in-game route agrees with the
	// loop's.
	//
	// NOT kFeatureNoQuit, which is the other way to satisfy the same conditions:
	// it also hides the Quit button on BOTH the ScummVM launcher (gui/launcher.cpp)
	// and the global main menu (engines/dialogs.cpp), and on this device quitting
	// ScummVM is the only way back to app_launcher and the native games.  That
	// would trap the user inside ScummVM — a worse bug than the one being fixed.
	// This option leaves the launcher's Quit button alone.
	if (!ConfMan.hasKey("gui_return_to_launcher_at_exit")) {
		ConfMan.setBool("gui_return_to_launcher_at_exit", true);
		configDirty = true;
	}

	if (configDirty)
		ConfMan.flushToDisk();

	// Call parent init
	ModularGraphicsBackend::initBackend();
	
	debug("Arca backend initialized");
}

Common::MutexInternal *OSystem_Arca::createMutex() {
	return new NullMutexInternal();
}

uint32 OSystem_Arca::getMillis(bool skipRecord) {
	timeval curTime;
	gettimeofday(&curTime, 0);
	
	// The multiply must happen in uint32, not in 32-bit signed time_t: at
	// 2147484 s of uptime (24.85 days) `sec_diff * 1000` overflows a signed
	// int, which is UB and in practice goes negative.  A wall display is never
	// power-cycled, so this is reachable, and it takes long-press detection,
	// cursor timing, the touch-feedback fade and DefaultTimerManager with it.
	// uint32 wraps cleanly at 49.7 days instead, which is what every
	// getMillis() consumer already assumes (they all do `now - _last`).
	// A negative usec delta wraps in the unsigned add and cancels correctly.
	return (uint32)(curTime.tv_sec - _startTime.tv_sec) * 1000u +
	       (uint32)((curTime.tv_usec - _startTime.tv_usec) / 1000);
}

void OSystem_Arca::delayMillis(uint msecs) {
	// Pump timer callbacks every 10 ms so OPL/MIDI sequencers advance
	// without a background thread (which deadlocks on single-core ARM).
	if (_timerManager)
		static_cast<DefaultTimerManager *>(_timerManager)->checkTimers(10);
	usleep(msecs * 1000);
}

void OSystem_Arca::getTimeAndDate(TimeDate &td, bool skipRecord) const {
	time_t curTime = time(0);
	struct tm t = *localtime(&curTime);
	td.tm_sec = t.tm_sec;
	td.tm_min = t.tm_min;
	td.tm_hour = t.tm_hour;
	td.tm_mday = t.tm_mday;
	td.tm_mon = t.tm_mon;
	td.tm_year = t.tm_year;
	td.tm_wday = t.tm_wday;
}

void OSystem_Arca::quit() {
	// Restore framebuffer to 32bpp before exiting so the app launcher (or any
	// respawned app) finds a clean 32bpp framebuffer.  closeFramebuffer() blanks
	// the screen, unmaps the fb, and calls fb_set_bpp("/dev/fb0", 32).
	//
	// The exit(0) is deliberate and is NOT what made leaving a game terminate
	// ScummVM — do not replace it with a _quit flag.  OSystem_SDL::quit() does
	// `destroy(); exit(0);` too, and nothing in ScummVM's game-exit path calls
	// this at all: the launcher loop in base/main.cpp decides, and it is
	// gui_return_to_launcher_at_exit that changes its mind (set in initBackend
	// above).  Exiting the process here is correct on this device — it is how the
	// init script's respawn returns the panel to app_launcher.
	if (_graphicsManager)
		((ArcaGraphicsManager *)_graphicsManager)->closeFramebuffer();
	exit(0);
}

// The base OSystem returns the bare relative name "scummvm.ini", which
// Common::FSNode resolves against the process's CURRENT DIRECTORY — and the cwd
// differs per launch method.  The boot init script does not cd and app_launcher
// execl()s without chdir(), so a boot-launched run used "/", an SSH-launched one
// used $HOME, and `cd /opt/games` first gave a third file.  RW09 accumulated
// three of them; settings did not follow the user between launch methods, and
// editing "the" ini was a coin flip.  OSystem_POSIX solves this with an absolute
// $HOME/.config path, but this backend derives from ModularGraphicsBackend, not
// from OSystem_POSIX, so it inherited the relative name instead.
//
// /opt/games is the natural home: it is next to the binary, the icons and the
// game data, it is writable, and it does not depend on $HOME — which the init
// script's environment does not set.
Common::String OSystem_Arca::getDefaultConfigFileName() {
	return "/opt/games/scummvm.ini";
}

static const char *logTypeTag(LogMessageType::Type type) {
	switch (type) {
	case LogMessageType::kInfo:    return "INFO ";
	case LogMessageType::kDebug:   return "DEBUG";
	case LogMessageType::kWarning: return "WARN ";
	case LogMessageType::kError:   return "ERROR";
	default:                       return "?????";
	}
}

void OSystem_Arca::logRotate() {
	if (!_logFile || _logBytes < 256 * 1024)
		return;
	fclose(LOG_FP);
	rename("/var/log/arca/scummvm.log",
	       "/var/log/arca/scummvm.log.1");
	_logFile = fopen("/var/log/arca/scummvm.log", "a");
	_logBytes = 0;
	if (_logFile)
		setvbuf(LOG_FP, nullptr, _IOLBF, 0);
}

void OSystem_Arca::logMessage(LogMessageType::Type type, const char *message) {
	if (type == LogMessageType::kDebug && !arcaDebugMode())
		return;

	// Write to log file with timestamp
	if (_logFile) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		struct tm tm;
		localtime_r(&ts.tv_sec, &tm);
		char timebuf[32];
		strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

		// Strip trailing newline from message (we add our own)
		size_t len = strlen(message);
		while (len > 0 && (message[len - 1] == '\n' || message[len - 1] == '\r'))
			len--;

		int n = fprintf(LOG_FP, "%s.%03ld [%s] scummvm        %.*s\n",
		                timebuf, ts.tv_nsec / 1000000,
		                logTypeTag(type),
		                (int)len, message);
		if (n > 0)
			_logBytes += n;
		logRotate();
	}

	// Also write to stderr/stdout for interactive use
	FILE *output = (type == LogMessageType::kInfo || type == LogMessageType::kDebug) ? stdout : stderr;
	fputs(message, output);
	fflush(output);
}

void OSystem_Arca::addSysArchivesToSearchSet(Common::SearchSet &s, int priority) {
	// Add any system-specific archive paths here if needed
}

bool OSystem_Arca::hasFeature(Feature f) {
#ifdef ENABLE_VKEYBD
	if (f == kFeatureVirtualKeyboard)
		return true;
#endif
	return ModularGraphicsBackend::hasFeature(f);
}

void OSystem_Arca::setFeatureState(Feature f, bool enable) {
#ifdef ENABLE_VKEYBD
	if (f == kFeatureVirtualKeyboard) {
		if (enable)
			showVirtualKeyboard();
		return;
	}
#endif
	ModularGraphicsBackend::setFeatureState(f, enable);
}

bool OSystem_Arca::getFeatureState(Feature f) {
#ifdef ENABLE_VKEYBD
	if (f == kFeatureVirtualKeyboard)
		return _vkbd && _vkbd->isLoaded();
#endif
	return ModularGraphicsBackend::getFeatureState(f);
}

void OSystem_Arca::showVirtualKeyboard() {
#ifdef ENABLE_VKEYBD
	if (!_vkbd) {
		_vkbd = new Common::VirtualKeyboard();
		// vkeybd_arca is a 2x-scaled (640x480) version of vkeybd_small,
		// sized for the device's 800x480 display.
		if (!_vkbd->loadKeyboardPack("vkeybd_arca") &&
		    !_vkbd->loadKeyboardPack("vkeybd_small") &&
		    !_vkbd->loadKeyboardPack("vkeybd_default")) {
			warning("Arca: failed to load vkeybd pack (deploy vkeybd_arca.zip to /opt/games/)");
			delete _vkbd;
			_vkbd = nullptr;
			return;
		}
	}
	_vkbd->show();
	debug("Arca: virtual keyboard shown");
#endif
}

// Factory function
OSystem *OSystem_Arca_create() {
	return new OSystem_Arca();
}

// Main entry point
int main(int argc, char *argv[]) {
	// Create the backend
	g_system = OSystem_Arca_create();
	assert(g_system);

	// Invoke ScummVM main
	int res = scummvm_main(argc, argv);
	
	// Cleanup
	g_system->destroy();
	
	return res;
}
