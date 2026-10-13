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

#ifndef BACKENDS_MIXER_OSS_H
#define BACKENDS_MIXER_OSS_H

#include "backends/mixer/mixer.h"
#include <pthread.h>

// Include C headers directly
extern "C" {
#include "audio_out.h"
/* ⚠️ NOT `#include "config.h"` — ScummVM generates its own config.h in its
 * build root, so that include resolves to ScummVM's from inside this tree.
 * One plain prototype, deliberately, and config.h says why. */
const char *config_audio_device_stored(void);
const char *config_audio_bt_addr_stored(void);
}

/**
 * Audio mixer that outputs to /dev/dsp, as a thin ADAPTER over the shared
 * device half in native_apps/common/audio_out.{c,h}.
 *
 * This class owns exactly three things: the ScummVM mixer, the fill callback
 * that hands its bytes to `AudioOut`, and the thread that services the stream.
 * Everything a device needs — the open, the format/rate/channel negotiation and the
 * read-back, the ring geometry, the lead, the prefill, the write policy and the
 * underrun accounting — belongs to `audio_out` and must not be reimplemented
 * here.  The next emulator port adapts the same way rather than writing a second
 * device backend.  Device facts: ../SYSTEM_ANALYSIS.md#34-audio.
 */
class OssMixerManager : public MixerManager {
public:
	OssMixerManager();
	~OssMixerManager() override;

	void init() override;
	void suspendAudio() override;
	int  resumeAudio() override;

	/** Audio thread entry-point (public so the C shim can call it). */
	void audioThread();

private:
	/** `AudioOutFill`: hands the interleaved device buffer to the mixer. */
	static long fillFromMixer(void *ctx, int16_t *buf, long frames, int channels);

	/** Open `_out` through `audio_out_open_resolved()` — at init and on every
	 *  reopen after an unplug or a replug.  Installs the fill once `_mixer` exists. */
	bool openDevice();

	AudioOut _out;          ///< The shared stream. The ONLY writer of /dev/dsp.
	uint32   _outputRate;   ///< What the device GRANTED — OPL tempo depends on it
	uint32   _samples;      ///< Frames per mixCallback call
	pthread_t _thread;
	volatile bool _threadRunning;
};

#endif // BACKENDS_MIXER_OSS_H
