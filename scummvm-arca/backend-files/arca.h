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

#ifndef BACKENDS_PLATFORM_ARCA_H
#define BACKENDS_PLATFORM_ARCA_H

#include "common/scummsys.h"
#include "backends/modular-backend.h"

// Forward declarations
class ArcaGraphicsManager;
class ArcaEventSource;
namespace Common { class VirtualKeyboard; }

class OSystem_Arca : public ModularMixerBackend, public ModularGraphicsBackend {
public:
	OSystem_Arca();
	virtual ~OSystem_Arca();

	virtual void initBackend() override;

	virtual Common::MutexInternal *createMutex() override;
	virtual uint32 getMillis(bool skipRecord = false) override;
	virtual void delayMillis(uint msecs) override;
	virtual void getTimeAndDate(TimeDate &td, bool skipRecord = false) const override;

	virtual void quit() override;

	// The config file lives at ONE absolute path, not wherever the process was
	// started from.  See getDefaultConfigFileName() in arca.cpp.
	virtual Common::String getDefaultConfigFileName() override;

	virtual void logMessage(LogMessageType::Type type, const char *message) override;

	virtual void addSysArchivesToSearchSet(Common::SearchSet &s, int priority) override;

	// Virtual keyboard support (requires build with --enable-vkeybd)
	virtual bool hasFeature(Feature f) override;
	virtual void setFeatureState(Feature f, bool enable) override;
	virtual bool getFeatureState(Feature f) override;
	void showVirtualKeyboard();

	// Access to the event source (screen geometry, game screen size)
	ArcaEventSource *getEventSource() const { return _eventSource; }

private:

	ArcaEventSource *_eventSource;
	timeval _startTime;

	// File logger — writes to /var/log/arca/scummvm.log
	// Stored as void* to avoid leaking <stdio.h> into other translation units
	// (which would conflict with ScummVM's FORBIDDEN_SYMBOL mechanism).
	void *_logFile;
	long  _logBytes;
	void  logRotate();

#ifdef ENABLE_VKEYBD
	Common::VirtualKeyboard *_vkbd;
#endif
};

// Returns true when ARCA_DEBUG env var is set (non-empty, non-"0").
// Checked once at first call and cached. Enables: touch-feedback circles,
// verbose touch-state logging.
bool arcaDebugMode();

// True when the GAME PICTURE may use the whole visible surface instead of the
// touch-safe rectangle. Set ARCA_CONTENT_AREA=visible for a one-off run,
// or arca_content_area=visible in scummvm.ini to persist it. Default: safe, i.e.
// every pixel of the picture is reachable by a finger.
// Checked once at first call and cached. The OVERLAY (launcher / GMM / virtual
// keyboard) always stays inside the safe rect and ignores this.
bool arcaFullContentArea();

// Cached pointer to the single backend instance.
// Set in OSystem_Arca constructor, avoids repeated dynamic_cast on g_system.
OSystem_Arca *arcaSystem();

#endif
