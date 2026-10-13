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

// These must come before any ScummVM header (forbidden.h is pulled in transitively).
#define FORBIDDEN_SYMBOL_EXCEPTION_unistd_h
#define FORBIDDEN_SYMBOL_EXCEPTION_time_h

#include "backends/mixer/oss/oss-mixer.h"
#include "audio/mixer_intern.h"
#include "common/debug.h"

#include <string.h>
#include <unistd.h>
#include <pthread.h>

// ---------------------------------------------------------------------------
// This file is an ADAPTER, and the point of it is what is NOT here
// ---------------------------------------------------------------------------
//
// It used to carry its own /dev/dsp open, its own ioctl order, its own ring-size
// query, its own silence prefill, its own EAGAIN retry loop, its own wall-clock
// deadline and its own emergency second write.  All of that is the DEVICE half,
// all of it is now audio_out.{c,h}, and there is one implementation of it because
// more emulator ports are coming: the next one adapts here rather than writing a
// third device backend.
//
// ⚠️ Two things about the old code are deliberately NOT carried over:
//
//   - The emergency anti-underrun write.  On a near-empty ring it mixed a second
//     buffer and wrote it while IGNORING the result — a partial write there
//     desynchronises the stream it was trying to rescue, and it was the second
//     write loop in a file that should have none.  `audio_out_service()` targets
//     the lead on every call, which is the same job done once and accounted for;
//     `audio_out_starved()` is where the count lives now.
//
//   - The fixed per-buffer deadline.  audio_out_service() writes a VARIABLE
//     frame count, so advancing a constant deadline against it is two pacing
//     models neither of which bounds the queue (audio_out.h says so about this
//     exact file).  The thread below drops the deadline and paces off the
//     library's own measured interval instead.
//
// ⚠️ Loudness is held IDENTICAL: audio_out_set_shift(&_out, 1) is the old `>>1`,
// and audio_out.h guarantees it is an arithmetic shift rather than a rounding
// multiply for exactly that reason.  ScummVM audio was verified good on the panel
// (King's Quest, Full Throttle) and this change must not be audible.

// ---------------------------------------------------------------------------
// C-linkage thread shim
// ---------------------------------------------------------------------------
static void *ossAudioThreadShim(void *arg) {
	static_cast<OssMixerManager *>(arg)->audioThread();
	return nullptr;
}

// ---------------------------------------------------------------------------
// OssMixerManager
// ---------------------------------------------------------------------------

OssMixerManager::OssMixerManager()
	: MixerManager()
	, _outputRate(22050)
	, _samples(2048)
	, _threadRunning(false) {
	memset(&_out, 0, sizeof(_out));
}

OssMixerManager::~OssMixerManager() {
	if (_threadRunning) {
		_threadRunning = false;
		pthread_join(_thread, nullptr);
	}
	// Remove the fill BEFORE closing: close() drains, and the mixer it would
	// call back into is destroyed by the base class right after this.
	if (audio_out_is_open(&_out))
		audio_out_set_fill(&_out, nullptr, nullptr, nullptr);
	audio_out_close(&_out);
}

// The fill contract speaks DEVICE FRAMES because of this call site: mixCallback
// wants a byte count.
//
// ⚠️ **The mixer is built MONO and the device may grant STEREO, so the buffer
// cannot be handed straight through.** `MixerImpl` is constructed with
// `stereo=false` below, so mixCallback emits one sample per frame whatever the
// device granted. Asking it for `frames * channels` samples and calling that
// done is what the previous version did, and on a device granting 2 it filled
// the buffer with 2*frames MONO samples that the DAC then drained as `frames`
// stereo frames — i.e. playback at double speed, an octave up, not silence and
// with no diagnostic on either side of the boundary. The onboard TWL4030 grants
// 2 as well; this went unnoticed only because nothing read the grant back.
//
// So: mix mono into the front of the buffer, then expand in place from the BACK,
// which is safe because `i * channels >= i` for every i. The buffer arrives
// zeroed and a short fill is legal, but mixCallback always fills what it is
// given, so this returns the whole request either way.
long OssMixerManager::fillFromMixer(void *ctx, int16_t *buf, long frames, int channels) {
	OssMixerManager *self = static_cast<OssMixerManager *>(ctx);
	if (!self->_mixer)
		return 0;

	self->_mixer->mixCallback((byte *)buf, (uint)(frames * 2));
	if (channels > 1) {
		for (long i = frames - 1; i >= 0; --i) {
			int16_t s = buf[i];
			for (int c = 0; c < channels; ++c)
				buf[i * channels + c] = s;
		}
	}
	return frames;
}

void OssMixerManager::init() {
	// ⚠️ **The device choice must be set before the open, and ScummVM has to do
	// this itself.** It links audio_out.o but NOT audio.o, so audio_init()'s
	// config read — the one the games get it from — is not on this path at all.
	// Without this line a USB DAC selected in Device Settings would work in
	// every game and silently not in ScummVM. config.o is already linked
	// (transitively, via hardware.o), so this costs no build change; the reason
	// it is a bare prototype rather than an include is in oss-mixer.h.
	audio_out_set_device_pref(config_audio_device_stored());
	audio_out_set_bt_addr(config_audio_bt_addr_stored());   // the headset USE FOR AUDIO pinned

	// ⚠️ channels_req stays 1.  audio_out.h: forcing stereo doubles this mixer's
	// work and its byte count on a core already at ~32 % with Full Throttle.
	// It is a REQUEST — see fillFromMixer() for what happens when it is not
	// granted (hw:0,0 is stereo-only; plughw may grant 1 and convert — either
	// grant is honoured).
	if (!openDevice()) {
		// No usable device — fall back to a silent mixer so ScummVM still works.
		warning("OssMixerManager: cannot open an output device (%s), audio disabled",
		        audio_out_backend_name());
		_mixer = new Audio::MixerImpl(_outputRate, false, _samples);
		_mixer->setReady(true);
		return;
	}

	_mixer = new Audio::MixerImpl(_outputRate, false, _samples);
	_mixer->setReady(true);

	// Install the fill only once _mixer exists — it is what the fill calls.
	audio_out_set_fill(&_out, fillFromMixer, this, "ScummVM");

	_threadRunning = true;
	pthread_create(&_thread, nullptr, ossAudioThreadShim, this);

	// NOTE: no SCHED_RR.  On this single 600 MHz core an RT-priority audio thread
	// starves the main thread during init and you get a black screen
	// (../CLAUDE.md → Cross-component build rules).
}

bool OssMixerManager::openDevice() {
	// Request what the mixer runs at: 22050 before init has built it, the granted
	// rate on every reopen, so a reopen asks for exactly what OPL is counting in.
	const char *path = nullptr;
	if (audio_out_open_resolved(&_out, (int)_outputRate, 1, &path) != 0)
		return false;

	// ⚠️ Use the GRANTED rate, never the requested one.  If _outputRate does not
	// match real playback, OPL sample-counting produces music at the wrong tempo.
	// A MixerImpl's rate is fixed at construction, so a REOPEN cannot adopt a
	// different grant — it says so rather than playing at the wrong pitch silently.
	int granted = audio_out_rate(&_out);
	if (!_mixer) {
		if (granted > 0)
			_outputRate = (uint32)granted;
	} else if (granted != (int)_outputRate) {
		warning("OssMixerManager: reopen granted %d Hz, mixer runs %u Hz — "
		        "pitch and tempo are off until ScummVM restarts",
		        granted, _outputRate);
	}

	// The old `>>1` speaker attenuation, bit for bit.  Every open resets it.
	audio_out_set_shift(&_out, 1);

	debug("OssMixerManager: %s via %s, %d Hz, %d ch, %d bit, %u frames/buf",
	      strcmp(audio_out_backend_name(), "alsa") == 0
	          ? audio_out_device_pcm(path) : path,
	      audio_out_backend_name(), granted, audio_out_channels(&_out),
	      audio_out_bits(&_out), _samples);

	// On a reopen the mixer already exists; at init, init() installs the fill
	// after it builds the mixer, because the fill is what calls it.
	if (_mixer)
		audio_out_set_fill(&_out, fillFromMixer, this, "ScummVM");
	return true;
}

void OssMixerManager::audioThread() {
	while (_threadRunning) {
		if (_audioSuspended) {
			usleep(50000);
			continue;
		}

		// ⚠️ An unplugged USB DAC must not silence the session, and a replugged
		// one must be taken back — the same two rules the games follow, decided
		// in audio_out (the lost flag, audio_out_usb_returned()) so there is one
		// implementation.  Only this thread touches _out after init, so the close
		// and reopen need no lock.  The fill is not called by the close.
		if (audio_out_device_lost(&_out)) {
			warning("OssMixerManager: output device lost — reopening");
			audio_out_close(&_out);
		} else if (audio_out_usb_returned(&_out)) {
			audio_out_close(&_out);
		}
		if (!audio_out_is_open(&_out) && !openDevice()) {
			// One attempt per second while nothing opens (the open prints its
			// reason each time), sliced so the destructor's join is not held.
			for (int i = 0; i < 20 && _threadRunning; ++i)
				usleep(50000);
			continue;
		}

		audio_out_service(&_out);

		// ⚠️ Pace at HALF the library's interval, not at it.
		//
		// audio_out_service_interval_us() is documented as the LONGEST a caller
		// may go between services — a ceiling, and the native path stays far
		// under it because it services from its render loop.  A dedicated thread
		// that sleeps the whole ceiling has no room left for the 20-40 ms of
		// scheduling jitter this core is measured to produce, so one late wakeup
		// starves the stream.  Half the ceiling means two services per budget
		// window and a single late wakeup cannot.
		//
		// ⚠️ But HALF is a choice, never measured.  Neither the whole ceiling nor
		// any other divisor has been tried; the stream is heard clean at half, and
		// that is the whole of the evidence.  It also stacks on a margin the library
		// already keeps (audio_out_service_interval_us() subtracts half a hardware
		// period internally) — though that one guards period granularity rather
		// than caller-side wakeup jitter, so the two are not obviously redundant.
		long budget = audio_out_service_interval_us(&_out);
		if (budget <= 0)
			budget = 20000;   // geometry not measured yet (pre-first-service)
		usleep((useconds_t)(budget / 2));
	}
}

void OssMixerManager::suspendAudio() {
	_audioSuspended = true;
}

int OssMixerManager::resumeAudio() {
	if (!_audioSuspended)
		return -2;
	_audioSuspended = false;
	return 0;
}
