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

// Forbidden symbol exceptions for file I/O used in loadBezelMargins()
#define FORBIDDEN_SYMBOL_EXCEPTION_FILE
#define FORBIDDEN_SYMBOL_EXCEPTION_fopen
// gettimeofday for frame-rate cap in updateScreen()
#define FORBIDDEN_SYMBOL_EXCEPTION_time_h
#define FORBIDDEN_SYMBOL_EXCEPTION_fclose
#define FORBIDDEN_SYMBOL_EXCEPTION_fgets
#define FORBIDDEN_SYMBOL_EXCEPTION_sscanf
#define FORBIDDEN_SYMBOL_EXCEPTION_fprintf

#include "backends/platform/arca/arca-graphics.h"
#include <sys/time.h>
#include "backends/platform/arca/arca.h"
#include "backends/platform/arca/arca-events.h"
extern "C" {
#include "hardware.h"
}
#include "common/rect.h"
#include "common/textconsole.h"
#include "common/system.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

ArcaGraphicsManager::ArcaGraphicsManager()
	: _fb(nullptr),
	  _fbInitialized(false),
	  _screenChangeID(0),
	  _screenWidth(0),
	  _screenHeight(0),
	  _screenFormat(Graphics::PixelFormat::createFormatCLUT8()),
	  _screenDirty(false),
	  _paletteDirty(false),
	  _cursorX(0),
	  _cursorY(0),
	  _cursorHotspotX(0),
	  _cursorHotspotY(0),
	  _cursorKeyColor(0),
	  _cursorVisible(false),
	  _cursorDontScale(false),
	  _cursorPaletteEnabled(false),
	  _prevDrawnCursorX(-1),
	  _prevDrawnCursorY(-1),
	  _prevDrawnCursorVisible(false),
	  _overlayVisible(false),
	  _overlayDirty(false),
	  _shakeXOffset(0),
	  _shakeYOffset(0),
	  _touchPointIndex(0) {

	memset(_palette, 0, sizeof(_palette));
	memset(_palette32, 0, sizeof(_palette32));
	memset(_cursorPalette, 0, sizeof(_cursorPalette));
	memset(_touchPoints, 0, sizeof(_touchPoints));
}

ArcaGraphicsManager::~ArcaGraphicsManager() {
	closeFramebuffer();
	_gameSurface.free();
	_cursorSurface.free();
	_overlaySurface.free();
}

void ArcaGraphicsManager::initFramebuffer() {
	if (_fbInitialized)
		return;

	// (O8) Switch to 16bpp RGB565 to halve memory bandwidth
	fb_set_bpp("/dev/fb0", 16);

	_fb = (Framebuffer *)malloc(sizeof(Framebuffer));
	if (!_fb) {
		warning("Failed to allocate framebuffer structure");
		return;
	}

	if (fb_init(_fb, "/dev/fb0") < 0) {
		warning("Failed to initialize framebuffer - device may not be available");
		free(_fb);
		_fb = nullptr;
		return;
	}

	// A game or video is watched, not touched: this port never blanks the panel.
	hw_blank_set_enabled(false);
	_fbInitialized = true;

	debug("Arca: framebuffer %dx%d visible", fbWidth(), fbHeight());
}

void ArcaGraphicsManager::closeFramebuffer() {
	if (_fbInitialized && _fb) {
		blankScreen();
		fb_close(_fb);
		free(_fb);
		_fb = nullptr;
		_fbInitialized = false;
		// (O8) Restore 32bpp for native games / game selector
		fb_set_bpp("/dev/fb0", 32);
	}
}

void ArcaGraphicsManager::blankScreen() {
	if (!_fbInitialized || !_fb)
		return;
	// memset works at any bpp (fb_clear assumes uint32 per pixel)
	memset(_fb->back_buffer, 0, _fb->back_buffer_size);
	fb_swap(_fb);
}

void ArcaGraphicsManager::getSafeRect(int &x, int &y, int &w, int &h) const {
	x = safeLeft();
	y = safeTop();
	w = safeWidth();
	h = safeHeight();
}

void ArcaGraphicsManager::getScalingInfo(int &scaledWidth, int &scaledHeight,
                                                int &offsetX, int &offsetY) const {
	// The framebuffer's logical size is already the visible rectangle: the bezel
	// is handled by fb_init()/fb_swap().  Inside that, the game is fitted to the
	// CONTENT rectangle — the touch-safe area by default, so that every pixel of
	// the picture can be reached by a finger (a game may put its verb bar or
	// inventory on the bottom row, and we cannot audit third-party content for
	// what has to be pressable).  arcaFullContentArea() opts out and hands the
	// picture the whole visible surface, dead band included.
	// Scale to fill the content rect preserving aspect ratio, and centre
	// whatever is left over within it.
	const bool full = arcaFullContentArea();
	const int rectX = full ? 0 : safeLeft();
	const int rectY = full ? 0 : safeTop();
	const int rectW = full ? fbWidth()  : safeWidth();
	const int rectH = full ? fbHeight() : safeHeight();

	if (_screenWidth == 0 || _screenHeight == 0) {
		scaledWidth  = rectW;
		scaledHeight = rectH;
		offsetX = rectX;
		offsetY = rectY;
		return;
	}

	int scaleX = rectW * 256 / _screenWidth;   // fixed-point *256
	int scaleY = rectH * 256 / _screenHeight;
	int scale  = (scaleX < scaleY) ? scaleX : scaleY;  // use smaller to fit

	scaledWidth  = (_screenWidth  * scale) / 256;
	scaledHeight = (_screenHeight * scale) / 256;

	offsetX = rectX + (rectW - scaledWidth)  / 2;
	offsetY = rectY + (rectH - scaledHeight) / 2;
}

bool ArcaGraphicsManager::hasFeature(OSystem::Feature f) const {
	return (f == OSystem::kFeatureCursorPalette);
}

void ArcaGraphicsManager::setFeatureState(OSystem::Feature f, bool enable) {
	if (f == OSystem::kFeatureCursorPalette)
		_cursorPaletteEnabled = enable;
}

bool ArcaGraphicsManager::getFeatureState(OSystem::Feature f) const {
	if (f == OSystem::kFeatureCursorPalette)
		return _cursorPaletteEnabled;
	return false;
}

#ifdef USE_RGB_COLOR
Graphics::PixelFormat ArcaGraphicsManager::getScreenFormat() const {
	return _screenFormat;
}

Common::List<Graphics::PixelFormat> ArcaGraphicsManager::getSupportedFormats() const {
	Common::List<Graphics::PixelFormat> list;
	// Arca framebuffer is 32-bit ARGB
	list.push_back(Graphics::PixelFormat(4, 8, 8, 8, 8, 16, 8, 0, 24)); // ARGB8888
	list.push_back(Graphics::PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0));  // RGB565
	list.push_back(Graphics::PixelFormat::createFormatCLUT8());          // CLUT8
	return list;
}
#endif

void ArcaGraphicsManager::initSize(uint width, uint height, const Graphics::PixelFormat *format) {
	debug("initSize: %dx%d", width, height);

	initFramebuffer();

	// BEFORE anything is sized from the touch-safe rectangle: this is what
	// republishes the geometry (and with it the measured touch inset) now that
	// fb_init() has run and the real logical dimensions are known.  The event
	// source is constructed before the framebuffer exists, so until this call
	// SCREEN_SAFE_* still reflects the pre-bezel defaults.
	OSystem_Arca *system = arcaSystem();
	if (system && system->getEventSource())
		system->getEventSource()->syncScreenGeometry();

	_screenWidth = width;
	_screenHeight = height;

	if (format) {
		_screenFormat = *format;
	} else {
		_screenFormat = Graphics::PixelFormat::createFormatCLUT8();
	}

	// Allocate game surface
	_gameSurface.create(width, height, _screenFormat);

	// Allocate overlay surface.  Sized to the TOUCH-SAFE rectangle, not the
	// whole visible framebuffer: the overlay is the ScummVM GUI — launcher,
	// GMM, virtual keyboard — which is nothing but buttons, so all of it has to
	// be reachable.  It must match getOverlayWidth()/getOverlayHeight() exactly,
	// because ThemeEngine and the virtual keyboard grabOverlay() into a surface
	// sized from those and copyRectToOverlay() it straight back.
	_overlaySurface.create(safeWidth(), safeHeight(),
	                       Graphics::PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0));
	// Pre-fill with transparent key so any stale pixels stay transparent
	clearOverlay();

	// Notify event source of the new game resolution and its placement.
	{
		int scaledW, scaledH, offX, offY;
		getScalingInfo(scaledW, scaledH, offX, offY);
		if (system && system->getEventSource())
			system->getEventSource()->setGameScreenSize(width, height, offX, offY);
	}

	// Fix 1: Increment screen change ID so ScummVM detects the mode transition.
	// Without this, DefaultEventManager may discard mouse events after initSize().
	_screenChangeID++;

	_screenDirty = true;
}

int ArcaGraphicsManager::getScreenChangeID() const {
	return _screenChangeID;
}

void ArcaGraphicsManager::beginGFXTransaction() {
	// No transaction support needed
}

OSystem::TransactionError ArcaGraphicsManager::endGFXTransaction() {
	return OSystem::kTransactionSuccess;
}

int16 ArcaGraphicsManager::getHeight() const {
	return _screenHeight;
}

int16 ArcaGraphicsManager::getWidth() const {
	return _screenWidth;
}

void ArcaGraphicsManager::setPalette(const byte *colors, uint start, uint num) {
	if (start + num > 256) {
		warning("setPalette: Invalid palette range");
		return;
	}

	memcpy(&_palette[start * 3], colors, num * 3);
	// Rebuild precomputed ARGB LUT for fast blit (O1)
	for (uint i = start; i < start + num; i++) {
		_palette32[i] = (0xFFu << 24)
		              | (_palette[i * 3 + 0] << 16)
		              | (_palette[i * 3 + 1] << 8)
		              |  _palette[i * 3 + 2];
		// (O8) Also build RGB565 LUT for 16bpp framebuffer
		_palette16[i] = ((_palette[i * 3 + 0] >> 3) << 11)
		              | ((_palette[i * 3 + 1] >> 2) << 5)
		              |  (_palette[i * 3 + 2] >> 3);
	}
	_paletteDirty = true;
	_screenDirty = true;
}

void ArcaGraphicsManager::grabPalette(byte *colors, uint start, uint num) const {
	if (start + num > 256) {
		warning("grabPalette: Invalid palette range");
		return;
	}

	memcpy(colors, &_palette[start * 3], num * 3);
}

uint32 ArcaGraphicsManager::convertColor(uint32 color, const Graphics::PixelFormat &srcFormat) {
	if (srcFormat.bytesPerPixel == 1) {
		// CLUT8 - use palette
		byte r = _palette[color * 3 + 0];
		byte g = _palette[color * 3 + 1];
		byte b = _palette[color * 3 + 2];
		return (0xFF << 24) | (r << 16) | (g << 8) | b;
	} else {
		// Convert from source format to ARGB8888
		byte r, g, b, a;
		srcFormat.colorToARGB(color, a, r, g, b);
		return (a << 24) | (r << 16) | (g << 8) | b;
	}
}

void ArcaGraphicsManager::copyRectToSurface(Graphics::Surface &dst, const void *buf,
                                                   int pitch, int x, int y, int w, int h,
                                                   const Graphics::PixelFormat &srcFormat) {
	if (x < 0 || y < 0 || x + w > dst.w || y + h > dst.h) {
		warning("copyRectToSurface: Rectangle out of bounds");
		return;
	}

	const byte *src = (const byte *)buf;

	if (srcFormat == dst.format) {
		// Direct copy
		for (int row = 0; row < h; row++) {
			memcpy((byte *)dst.getBasePtr(x, y + row), src + row * pitch, w * dst.format.bytesPerPixel);
		}
	} else {
		// Format conversion needed
		for (int row = 0; row < h; row++) {
			for (int col = 0; col < w; col++) {
				uint32 srcColor;
				if (srcFormat.bytesPerPixel == 1) {
					srcColor = src[row * pitch + col];
				} else if (srcFormat.bytesPerPixel == 2) {
					srcColor = *(const uint16 *)(src + row * pitch + col * 2);
				} else {
					srcColor = *(const uint32 *)(src + row * pitch + col * 4);
				}

				uint32 dstColor = convertColor(srcColor, srcFormat);

				if (dst.format.bytesPerPixel == 4) {
					*(uint32 *)dst.getBasePtr(x + col, y + row) = dstColor;
				} else if (dst.format.bytesPerPixel == 2) {
					byte r = (dstColor >> 16) & 0xFF;
					byte g = (dstColor >> 8) & 0xFF;
					byte b = dstColor & 0xFF;
					*(uint16 *)dst.getBasePtr(x + col, y + row) =
						((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
				}
			}
		}
	}
}

void ArcaGraphicsManager::copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) {
	if (!_gameSurface.getPixels()) {
		warning("copyRectToScreen: Game surface not initialized");
		return;
	}

	copyRectToSurface(_gameSurface, buf, pitch, x, y, w, h, _screenFormat);
	_screenDirty = true;
}

Graphics::Surface *ArcaGraphicsManager::lockScreen() {
	return &_gameSurface;
}

void ArcaGraphicsManager::unlockScreen() {
	_screenDirty = true;
}

void ArcaGraphicsManager::fillScreen(uint32 col) {
	if (!_gameSurface.getPixels())
		return;

	uint32 color = convertColor(col, _screenFormat);

	if (_gameSurface.format.bytesPerPixel == 4) {
		uint32 *pixels = (uint32 *)_gameSurface.getPixels();
		for (int i = 0; i < _gameSurface.w * _gameSurface.h; i++) {
			pixels[i] = color;
		}
	} else if (_gameSurface.format.bytesPerPixel == 2) {
		uint16 *pixels = (uint16 *)_gameSurface.getPixels();
		uint16 color16 = (uint16)color;
		for (int i = 0; i < _gameSurface.w * _gameSurface.h; i++) {
			pixels[i] = color16;
		}
	} else {
		byte *pixels = (byte *)_gameSurface.getPixels();
		for (int i = 0; i < _gameSurface.w * _gameSurface.h; i++) {
			pixels[i] = (byte)col;
		}
	}

	_screenDirty = true;
}

void ArcaGraphicsManager::fillScreen(const Common::Rect &r, uint32 col) {
	if (!_gameSurface.getPixels())
		return;

	uint32 color = convertColor(col, _screenFormat);

	for (int y = r.top; y < r.bottom; y++) {
		if (_gameSurface.format.bytesPerPixel == 4) {
			uint32 *row = (uint32 *)_gameSurface.getBasePtr(r.left, y);
			for (int x = 0; x < r.width(); x++) {
				row[x] = color;
			}
		} else if (_gameSurface.format.bytesPerPixel == 2) {
			uint16 *row = (uint16 *)_gameSurface.getBasePtr(r.left, y);
			uint16 color16 = (uint16)color;
			for (int x = 0; x < r.width(); x++) {
				row[x] = color16;
			}
		} else {
			byte *row = (byte *)_gameSurface.getBasePtr(r.left, y);
			for (int x = 0; x < r.width(); x++) {
				row[x] = (byte)col;
			}
		}
	}

	_screenDirty = true;
}

void ArcaGraphicsManager::blitGameSurfaceToFramebuffer() {
	if (!_fbInitialized || !_fb || !_gameSurface.getPixels())
		return;

	// Scaled region within the visible framebuffer
	int scaledW, scaledH, offsetX, offsetY;
	getScalingInfo(scaledW, scaledH, offsetX, offsetY);

	// Apply shake offset
	offsetX += _shakeXOffset;
	offsetY += _shakeYOffset;

	const int fbW = fbWidth();
	const int fbH = fbHeight();

	// (O3+O8) Clear only the border strips — use uint16* for 16bpp fb
	{
		uint16 *buf16 = (uint16 *)_fb->back_buffer;
		// Top strip
		int topRows = (offsetY > 0) ? offsetY : 0;
		if (topRows > 0)
			memset(buf16, 0, topRows * fbW * 2);
		// Bottom strip
		int botStart = offsetY + scaledH;
		if (botStart < fbH)
			memset(buf16 + botStart * fbW, 0, (fbH - botStart) * fbW * 2);
		// Left strip (within scaled rows)
		int leftCols = (offsetX > 0) ? offsetX : 0;
		int rightStart = offsetX + scaledW;
		int rightCols = (rightStart < fbW) ? (fbW - rightStart) : 0;
		if (leftCols > 0 || rightCols > 0) {
			for (int y = topRows; y < botStart && y < fbH; y++) {
				if (leftCols > 0)
					memset(buf16 + y * fbW, 0, leftCols * 2);
				if (rightCols > 0)
					memset(buf16 + y * fbW + rightStart, 0, rightCols * 2);
			}
		}
	}

	// (O2) Precompute X-coordinate lookup table to eliminate per-pixel division
	int srcXtab[kPanelWidth];
	for (int dx = 0; dx < scaledW && dx < fbW; dx++) {
		int sx = (dx * (int)_screenWidth) / scaledW;
		srcXtab[dx] = (sx < (int)_screenWidth) ? sx : (int)_screenWidth - 1;
	}

	const int bpp = _screenFormat.bytesPerPixel;

	// (O11) Row deduplication via cached temp row.
	// When scaling 200→~460 rows (nearest-neighbour), ~57% of consecutive
	// dest rows map to the same source row.  We render unique rows into a
	// stack-resident temp buffer (L1-cache-hot), then memcpy to the
	// framebuffer.  Duplicate rows just repeat the memcpy — skipping
	// palette lookup + NEON entirely.  Using a cached temp buffer avoids
	// reading from the framebuffer's write-combined memory (very slow on ARM).
	uint16 tempRow[kPanelWidth];
	int cpySrc = (offsetX < 0) ? 0 : offsetX;
	int cpyEnd = ((offsetX + scaledW) > fbW) ? fbW : (offsetX + scaledW);
	int cpyBytes = (cpyEnd > cpySrc) ? (cpyEnd - cpySrc) * 2 : 0;
	int prevSrcY = -1;

	// Scale game surface → framebuffer using nearest-neighbour (O8: 16bpp output)
	for (int dy = 0; dy < scaledH; dy++) {
		int fbY = offsetY + dy;
		if (fbY < 0 || fbY >= fbH)
			continue;

		// Map destination row back to source row
		int srcY = (dy * (int)_screenHeight) / scaledH;
		if (srcY >= (int)_screenHeight) srcY = _screenHeight - 1;

		uint16 *fbRow = (uint16 *)_fb->back_buffer + fbY * fbW;

		// (O2) Lift row pointer once per source row
		if (bpp == 1) {
			// (O11) Only render unique source rows; duplicates reuse tempRow
			if (srcY != prevSrcY) {
				prevSrcY = srcY;
				// (O1+O8+O10) CLUT8 fast path: render to cached tempRow
				const byte *srcRow = (const byte *)_gameSurface.getBasePtr(0, srcY);
				int dxStart = (offsetX < 0) ? -offsetX : 0;
				int dxEnd   = (offsetX + scaledW > fbW) ? (fbW - offsetX) : scaledW;
				uint16 *dst = tempRow + offsetX + dxStart;
				int dx = dxStart;
#ifdef __ARM_NEON
				// (O8+O10) NEON: write 8 pixels (16 bytes) per iteration at 16bpp
				int dxStop8 = dxStart + ((dxEnd - dxStart) & ~7);
				for (; dx < dxStop8; dx += 8) {
					uint16x8_t px = vdupq_n_u16(0);
					px = vsetq_lane_u16(_palette16[srcRow[srcXtab[dx + 0]]], px, 0);
					px = vsetq_lane_u16(_palette16[srcRow[srcXtab[dx + 1]]], px, 1);
					px = vsetq_lane_u16(_palette16[srcRow[srcXtab[dx + 2]]], px, 2);
					px = vsetq_lane_u16(_palette16[srcRow[srcXtab[dx + 3]]], px, 3);
					px = vsetq_lane_u16(_palette16[srcRow[srcXtab[dx + 4]]], px, 4);
					px = vsetq_lane_u16(_palette16[srcRow[srcXtab[dx + 5]]], px, 5);
					px = vsetq_lane_u16(_palette16[srcRow[srcXtab[dx + 6]]], px, 6);
					px = vsetq_lane_u16(_palette16[srcRow[srcXtab[dx + 7]]], px, 7);
					vst1q_u16(dst, px);
					dst += 8;
				}
#endif
				// Scalar remainder (or full loop if no NEON)
				for (; dx < dxEnd; dx++)
					*dst++ = _palette16[srcRow[srcXtab[dx]]];
			}
			// Copy cached row to framebuffer (write-only, no fb read)
			if (cpyBytes > 0)
				memcpy(fbRow + cpySrc, tempRow + cpySrc, cpyBytes);
		} else if (bpp == 2) {
			// Source is already 16-bit — convert via colorToARGB then to RGB565
			const uint16 *srcRow = (const uint16 *)_gameSurface.getBasePtr(0, srcY);
			for (int dx = 0; dx < scaledW; dx++) {
				int fbX = offsetX + dx;
				if (fbX < 0 || fbX >= fbW) continue;
				uint16 pixel = srcRow[srcXtab[dx]];
				byte r, g, b, a;
				_screenFormat.colorToARGB(pixel, a, r, g, b);
				fbRow[fbX] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
			}
		} else {
			// 32bpp source → RGB565
			const uint32 *srcRow = (const uint32 *)_gameSurface.getBasePtr(0, srcY);
			for (int dx = 0; dx < scaledW; dx++) {
				int fbX = offsetX + dx;
				if (fbX < 0 || fbX >= fbW) continue;
				uint32 pixel = srcRow[srcXtab[dx]];
				byte r, g, b, a;
				_screenFormat.colorToARGB(pixel, a, r, g, b);
				fbRow[fbX] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
			}
		}
	}
}

void ArcaGraphicsManager::drawCursor() {
	if (!_cursorVisible || !_cursorSurface.getPixels() || !_fb)
		return;

	// Fix 3: Scale cursor position from game coordinates to framebuffer coordinates
	// when in game mode. In overlay mode, cursor coords are already in screen space.
	int cursorX, cursorY;
	if (!_overlayVisible && _screenWidth > 0 && _screenHeight > 0) {
		// Game mode: _cursorX/_cursorY are in game coords (e.g. 0-319 for 320-wide).
		// We must scale them to the framebuffer's scaled region.
		int scaledW, scaledH, offsetX, offsetY;
		getScalingInfo(scaledW, scaledH, offsetX, offsetY);
		cursorX = _cursorX * scaledW / (int)_screenWidth + offsetX - _cursorHotspotX;
		cursorY = _cursorY * scaledH / (int)_screenHeight + offsetY - _cursorHotspotY;
	} else {
		// Overlay mode: cursor coordinates are in OVERLAY space, which is the
		// touch-safe rectangle — so shift by its origin to get framebuffer
		// coordinates.
		cursorX = _cursorX - _cursorHotspotX + safeLeft();
		cursorY = _cursorY - _cursorHotspotY + safeTop();
	}

	uint16 *buf16 = (uint16 *)_fb->back_buffer;
	const int fbW = fbWidth();
	const int fbH = fbHeight();

	for (int y = 0; y < _cursorSurface.h; y++) {
		int fbY = cursorY + y;
		if (fbY < 0 || fbY >= fbH)
			continue;

		for (int x = 0; x < _cursorSurface.w; x++) {
			int fbX = cursorX + x;
			if (fbX < 0 || fbX >= fbW)
				continue;

			byte r, g, b;
			if (_cursorSurface.format.bytesPerPixel == 1) {
				byte index = *(const byte *)_cursorSurface.getBasePtr(x, y);
				if (index == _cursorKeyColor)
					continue;

				byte *pal = _cursorPaletteEnabled ? _cursorPalette : _palette;
				r = pal[index * 3 + 0];
				g = pal[index * 3 + 1];
				b = pal[index * 3 + 2];
			} else if (_cursorSurface.format.bytesPerPixel == 2) {
				// Fix 4: Handle 16bpp cursor format properly.
				// Reading 4 bytes for a 2bpp cursor causes memory corruption.
				uint16 pixel = *(const uint16 *)_cursorSurface.getBasePtr(x, y);
				if ((uint32)pixel == _cursorKeyColor)
					continue;

				byte a;
				_cursorSurface.format.colorToARGB(pixel, a, r, g, b);
			} else {
				uint32 pixel = *(const uint32 *)_cursorSurface.getBasePtr(x, y);
				if (pixel == _cursorKeyColor)
					continue;

				byte a;
				_cursorSurface.format.colorToARGB(pixel, a, r, g, b);
			}

			buf16[fbY * fbW + fbX] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		}
	}
}

void ArcaGraphicsManager::updateScreen() {
	if (!_fbInitialized || !_fb) {
		static int _updateSkipCount = 0;
		if (++_updateSkipCount == 100)
			warning("Arca: updateScreen called 100x but fb not initialized");
		return;
	}

	// Fix 2: Restructured update loop.  The cursor moves independently of
	// game content, so we must never skip drawCursor() or fb_swap().
	// We still rate-limit to ~30 fps, but check whether the cursor moved
	// to force a redraw even when the game surface is clean.

	// FIRST: sync cursor position from event manager so cursorMoved
	// detection below uses the CURRENT position, not the stale one.
	if (g_system && g_system->getEventManager()) {
		Common::Point mousePos = g_system->getEventManager()->getMousePos();
		_cursorX = mousePos.x;
		_cursorY = mousePos.y;
	}

	// Detect whether the cursor changed since the last drawn frame
	bool cursorMoved = (_cursorX != _prevDrawnCursorX ||
	                    _cursorY != _prevDrawnCursorY ||
	                    _cursorVisible != _prevDrawnCursorVisible);

	// Nothing changed: leave the framebuffer alone entirely.  This test used
	// _overlayVisible rather than _overlayDirty, and because the launcher IS the
	// overlay it was permanently true — so the cap below never applied and a
	// static screen recomposited 800x455 as fast as the GUI could call
	// updateScreen(), pinning ~52 % of the single core with nothing happening.
	// The dirty flags are only cleared once a frame is really drawn, so a change
	// skipped here stays pending for the next call.
	bool needsRedraw = _screenDirty || _overlayDirty || cursorMoved;
	if (!needsRedraw)
		return;

	// Cap at 30 fps (~33 ms) to avoid burning the 600 MHz ARM CPU.
	// NOTE: _lastFrame is initialised on first call, NOT to {0,0}.  On
	// 32-bit ARM `long` is 32 bits; subtracting epoch 0 from the current
	// time and multiplying by 1 000 000 overflows to a negative value,
	// which always passes the < 33333 gate — every frame is skipped.
	static struct timeval _lastFrame = {0, 0};
	static bool _firstFrame = true;
	struct timeval _now;
	gettimeofday(&_now, nullptr);
	if (_firstFrame) {
		_firstFrame = false;
		_lastFrame = _now;
		// fall through to render the first frame immediately
	} else {
		long _elapsedUs = (_now.tv_sec - _lastFrame.tv_sec) * 1000000L
		                + (_now.tv_usec - _lastFrame.tv_usec);
		if (_elapsedUs > 0 && _elapsedUs < 33333L)
			return;
		_lastFrame = _now;
	}

	if (_overlayVisible) {
		// Always draw the game surface first as background so the overlay
		// (e.g. virtual keyboard) appears on top of the game rather than
		// replacing it.
		blitGameSurfaceToFramebuffer();
		_screenDirty = false;

		// Composite overlay on top: treat 0xF81F (magenta, clear-key) pixels as
		// transparent so only the actual overlay bitmap is visible.  Black and
		// all other real colours are composited opaquely, which fixes the GMM
		// background and the VKB text input field showing as transparent.
		// (O8) Overlay is already RGB565 — write directly to 16bpp back buffer.
		// The overlay surface is the size of the touch-safe rectangle, so it is
		// blitted at that rectangle's origin; blitGameSurfaceToFramebuffer()
		// above has already blacked everything outside the game's own rect.
		uint16 *obuf16 = (uint16 *)_fb->back_buffer;
		const int fbW = fbWidth();
		const int ox = safeLeft();
		const int oy = safeTop();
		const int oh = (_overlaySurface.h < safeHeight()) ? _overlaySurface.h : safeHeight();
		const int ow = (_overlaySurface.w < safeWidth())  ? _overlaySurface.w : safeWidth();
		for (int y = 0; y < oh; y++) {
			for (int x = 0; x < ow; x++) {
				uint16 pixel = *(const uint16 *)_overlaySurface.getBasePtr(x, y);
				if (pixel == 0xF81F)
					continue; // transparent clear-key – keep game pixel underneath
				obuf16[(y + oy) * fbW + (x + ox)] = pixel;
			}
		}
		_overlayDirty = false;
	} else {
		// Always re-blit the game surface.  Even when _screenDirty is false
		// we need to erase the previous cursor position before drawing the
		// cursor at its new location.
		blitGameSurfaceToFramebuffer();
		_screenDirty = false;
	}

	// Draw touch feedback (debug mode only: set ARCA_DEBUG=1)
	if (arcaDebugMode())
		drawTouchFeedback();

	// Draw cursor on top
	drawCursor();

	// Update previous cursor state for next frame's dirty check
	_prevDrawnCursorX = _cursorX;
	_prevDrawnCursorY = _cursorY;
	_prevDrawnCursorVisible = _cursorVisible;

	// Swap buffers
	fb_swap(_fb);
}

void ArcaGraphicsManager::setShakePos(int shakeXOffset, int shakeYOffset) {
	_shakeXOffset = shakeXOffset;
	_shakeYOffset = shakeYOffset;
	_screenDirty = true;
}

void ArcaGraphicsManager::setFocusRectangle(const Common::Rect& rect) {
	// Not implemented
}

void ArcaGraphicsManager::clearFocusRectangle() {
	// Not implemented
}

// Overlay methods
void ArcaGraphicsManager::showOverlay(bool inGUI) {
	_overlayVisible = true;
	_overlayDirty = true;      // first frame with the overlay up must composite
}

void ArcaGraphicsManager::hideOverlay() {
	_overlayVisible = false;
	_screenDirty = true;
}

bool ArcaGraphicsManager::isOverlayVisible() const {
	return _overlayVisible;
}

Graphics::PixelFormat ArcaGraphicsManager::getOverlayFormat() const {
	return Graphics::PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0);
}

void ArcaGraphicsManager::clearOverlay() {
	if (_overlaySurface.getPixels()) {
		// Fill with the transparent clear-key (0xF81F, magenta) rather than
		// zero/black.  This lets true black and dark-grey GUI backgrounds
		// render opaquely while still allowing the game to show through any
		// uncovered overlay area (e.g. around the VKB).
		uint16 *p = (uint16 *)_overlaySurface.getPixels();
		const int count = _overlaySurface.w * _overlaySurface.h;
		for (int i = 0; i < count; i++)
			p[i] = 0xF81F;
		_overlayDirty = true;
	}
}

void ArcaGraphicsManager::grabOverlay(Graphics::Surface &surface) const {
	surface.copyFrom(_overlaySurface);
}

void ArcaGraphicsManager::copyRectToOverlay(const void *buf, int pitch, int x, int y, int w, int h) {
	if (!_overlaySurface.getPixels())
		return;

	copyRectToSurface(_overlaySurface, buf, pitch, x, y, w, h, getOverlayFormat());
	_overlayDirty = true;
}

int16 ArcaGraphicsManager::getOverlayHeight() const {
	// The overlay is the GUI: all of it must be pressable, so it gets the
	// touch-safe rectangle and not the whole visible surface.
	return (int16)safeHeight();
}

int16 ArcaGraphicsManager::getOverlayWidth() const {
	return (int16)safeWidth();
}

// Mouse cursor methods
bool ArcaGraphicsManager::showMouse(bool visible) {
	bool wasVisible = _cursorVisible;
	_cursorVisible = visible;
	return wasVisible;
}

void ArcaGraphicsManager::warpMouse(int x, int y) {
	_cursorX = x;
	_cursorY = y;
}

void ArcaGraphicsManager::setMouseCursor(const void *buf, uint w, uint h,
                                                int hotspotX, int hotspotY,
                                                uint32 keycolor, bool dontScale,
                                                const Graphics::PixelFormat *format,
                                                const byte *mask) {
	_cursorHotspotX = hotspotX;
	_cursorHotspotY = hotspotY;
	_cursorKeyColor = keycolor;
	_cursorDontScale = dontScale;

	Graphics::PixelFormat cursorFormat = format ? *format : Graphics::PixelFormat::createFormatCLUT8();

	_cursorSurface.create(w, h, cursorFormat);
	memcpy(_cursorSurface.getPixels(), buf, w * h * cursorFormat.bytesPerPixel);
	// The cursor bitmap changed without the cursor moving, so the dirty test in
	// updateScreen() would not otherwise notice.  Rare enough that a full reblit
	// costs nothing.
	_screenDirty = true;
}

void ArcaGraphicsManager::setCursorPalette(const byte *colors, uint start, uint num) {
	if (start + num > 256) {
		warning("setCursorPalette: Invalid palette range");
		return;
	}

	memcpy(&_cursorPalette[start * 3], colors, num * 3);
	_cursorPaletteEnabled = true;
	_screenDirty = true;
}

void ArcaGraphicsManager::addTouchPoint(int x, int y) {
	uint32 currentTime = g_system->getMillis();

	// Add new touch point
	_touchPoints[_touchPointIndex].x = x;
	_touchPoints[_touchPointIndex].y = y;
	_touchPoints[_touchPointIndex].timestamp = currentTime;
	_touchPoints[_touchPointIndex].active = true;

	_touchPointIndex = (_touchPointIndex + 1) % MAX_TOUCH_POINTS;
}

void ArcaGraphicsManager::drawTouchFeedback() {
	if (!_fb)
		return;

	uint16 *buf16 = (uint16 *)_fb->back_buffer;
	const int fbW = fbWidth();
	const int fbH = fbHeight();
	uint32 currentTime = g_system->getMillis();
	const uint32 FADE_TIME = 1000; // 1 second fade

	// Draw circles for recent touch points
	for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
		if (!_touchPoints[i].active)
			continue;

		uint32 age = currentTime - _touchPoints[i].timestamp;
		if (age > FADE_TIME) {
			_touchPoints[i].active = false;
			continue;
		}

		// Calculate alpha based on age (fade out)
		int alpha = 255 - (age * 255 / FADE_TIME);

		// Draw circle at touch point
		int cx = _touchPoints[i].x;
		int cy = _touchPoints[i].y;
		int radius = 20;

		// Draw filled circle (O8: RGB565)
		for (int dy = -radius; dy <= radius; dy++) {
			for (int dx = -radius; dx <= radius; dx++) {
				if (dx * dx + dy * dy <= radius * radius) {
					int px = cx + dx;
					int py = cy + dy;

					if (px >= 0 && px < fbW && py >= 0 && py < fbH) {
						// Blend red circle with existing RGB565 pixel
						uint16 existing = buf16[py * fbW + px];
						byte er = ((existing >> 11) & 0x1F) << 3;
						byte eg = ((existing >> 5) & 0x3F) << 2;
						byte eb = (existing & 0x1F) << 3;

						byte r = ((255 * alpha) + (er * (255 - alpha))) / 255;
						byte g = ((0 * alpha) + (eg * (255 - alpha))) / 255;
						byte b = ((0 * alpha) + (eb * (255 - alpha))) / 255;

						buf16[py * fbW + px] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
					}
				}
			}
		}
	}
}
