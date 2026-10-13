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

#ifndef BACKENDS_GRAPHICS_ARCA_H
#define BACKENDS_GRAPHICS_ARCA_H

#include "backends/graphics/graphics.h"
#include "graphics/surface.h"

// Include C headers directly
extern "C" {
#include "framebuffer.h"
}

class ArcaGraphicsManager : public GraphicsManager {
public:
	ArcaGraphicsManager();
	virtual ~ArcaGraphicsManager();

	// GraphicsManager API
	bool hasFeature(OSystem::Feature f) const override;
	void setFeatureState(OSystem::Feature f, bool enable) override;
	bool getFeatureState(OSystem::Feature f) const override;

#ifdef USE_RGB_COLOR
	Graphics::PixelFormat getScreenFormat() const override;
	Common::List<Graphics::PixelFormat> getSupportedFormats() const override;
#endif

	void initSize(uint width, uint height, const Graphics::PixelFormat *format = NULL) override;
	int getScreenChangeID() const override;

	void beginGFXTransaction() override;
	OSystem::TransactionError endGFXTransaction() override;

	int16 getHeight() const override;
	int16 getWidth() const override;

	void setPalette(const byte *colors, uint start, uint num) override;
	void grabPalette(byte *colors, uint start, uint num) const override;

	void copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) override;
	Graphics::Surface *lockScreen() override;
	void unlockScreen() override;

	void fillScreen(uint32 col) override;
	void fillScreen(const Common::Rect &r, uint32 col) override;
	void updateScreen() override;

	void setShakePos(int shakeXOffset, int shakeYOffset) override;
	void setFocusRectangle(const Common::Rect& rect) override;
	void clearFocusRectangle() override;

	// Overlay
	void showOverlay(bool inGUI) override;
	void hideOverlay() override;
	bool isOverlayVisible() const override;
	Graphics::PixelFormat getOverlayFormat() const override;
	void clearOverlay() override;
	void grabOverlay(Graphics::Surface &surface) const override;
	void copyRectToOverlay(const void *buf, int pitch, int x, int y, int w, int h) override;
	int16 getOverlayHeight() const override;
	int16 getOverlayWidth() const override;

	// Mouse cursor
	bool showMouse(bool visible) override;
	void warpMouse(int x, int y) override;
	void setMouseCursor(const void *buf, uint w, uint h, int hotspotX, int hotspotY,
	                    uint32 keycolor, bool dontScale = false,
	                    const Graphics::PixelFormat *format = NULL, const byte *mask = NULL) override;
	void setCursorPalette(const byte *colors, uint start, uint num) override;

private:
	// Framebuffer
	Framebuffer *_fb;
	bool _fbInitialized;

	// Screen change tracking (incremented on each initSize call)
	int _screenChangeID;

	// Screen properties
	uint _screenWidth;
	uint _screenHeight;
	Graphics::PixelFormat _screenFormat;
	Graphics::Surface _gameSurface;
	bool _screenDirty;

	// The framebuffer's LOGICAL size — the visible rectangle inside the bezel.
	// fb_init() sizes it and fb_swap() places it on the panel, so everything
	// here draws in logical coordinates and no bezel arithmetic is needed.
	//
	// These remain the STRIDE and the BOUNDS of the back buffer, and nothing
	// else. They are NOT where content goes: see safeWidth() below.
	int fbWidth() const  { return _fb ? (int)_fb->width  : kPanelWidth; }
	int fbHeight() const { return _fb ? (int)_fb->height : kPanelHeight; }

	// The touch-safe rectangle: visible AND reachable by a finger. The digitiser
	// saturates before the panel edge, so a band at each end of Y (and a few
	// columns on X) is drawable but unpressable — measured at runtime by
	// touch_input.c's publish_safe_area(), zero until a panel has been swept.
	//
	// The OVERLAY lives here unconditionally (launcher, GMM, virtual keyboard —
	// all buttons). The GAME PICTURE lives here too by default, because a game's
	// verb bar or inventory may sit on its bottom row and we cannot audit
	// third-party content for what has to be pressable; arcaFullContentArea()
	// opts out and gives the picture the whole surface instead.
	//
	// Only correct after touch_init(), which is why initSize() calls
	// syncScreenGeometry() before sizing anything from these.
	int safeLeft() const   { return SCREEN_SAFE_LEFT; }
	int safeTop() const    { return SCREEN_SAFE_TOP; }
	int safeWidth() const  { return SCREEN_SAFE_WIDTH; }
	int safeHeight() const { return SCREEN_SAFE_HEIGHT; }

	// Panel size, and therefore the upper bound on any logical dimension.
	static const int kPanelWidth  = 800;
	static const int kPanelHeight = 480;

	// Palette for CLUT8 mode
	byte _palette[256 * 3];
	uint32 _palette32[256]; // precomputed ARGB for fast blit (O1)
	uint16 _palette16[256]; // precomputed RGB565 for 16bpp fb (O8)
	bool _paletteDirty;

	// Cursor
	Graphics::Surface _cursorSurface;
	int _cursorX, _cursorY;
	int _cursorHotspotX, _cursorHotspotY;
	uint32 _cursorKeyColor;
	bool _cursorVisible;
	bool _cursorDontScale;
	byte _cursorPalette[256 * 3];
	bool _cursorPaletteEnabled;

	// Previous cursor state for dirty tracking
	int _prevDrawnCursorX;
	int _prevDrawnCursorY;
	bool _prevDrawnCursorVisible;

	// Overlay
	Graphics::Surface _overlaySurface;
	bool _overlayVisible;
	/* Overlay CONTENT changed, as opposed to the overlay merely being up.
	 * updateScreen() used _overlayVisible in its dirty test, and since the
	 * ScummVM launcher *is* the overlay that made the flag permanently true —
	 * the 30 fps cap never applied and a static launcher screen recomposited
	 * the whole 800x455 surface as fast as the GUI could call, ~52 % of the
	 * single core. Set by showOverlay/clearOverlay/copyRectToOverlay, cleared
	 * when a frame is actually drawn. */
	bool _overlayDirty;

	// Shake offset
	int _shakeXOffset;
	int _shakeYOffset;

	// Touch feedback visualization
	struct TouchPoint {
		int x, y;
		uint32 timestamp;
		bool active;
	};
	static const int MAX_TOUCH_POINTS = 10;
	TouchPoint _touchPoints[MAX_TOUCH_POINTS];
	int _touchPointIndex;

	// Helper methods
	void initFramebuffer();
	void blitGameSurfaceToFramebuffer();
	void drawCursor();
	void drawTouchFeedback();
	uint32 convertColor(uint32 color, const Graphics::PixelFormat &srcFormat);
	void copyRectToSurface(Graphics::Surface &dst, const void *buf, int pitch,
	                       int x, int y, int w, int h, const Graphics::PixelFormat &srcFormat);

public:
	// Touch feedback API
	void addTouchPoint(int x, int y);

	// Scaling info for coordinate transformation
	void getScalingInfo(int &scaledWidth, int &scaledHeight, int &offsetX, int &offsetY) const;

	// The touch-safe rectangle in logical coordinates, for the event source:
	// the overlay's origin/size and the gesture corners must agree with what is
	// drawn here, so both read it from one place rather than the macros twice.
	void getSafeRect(int &x, int &y, int &w, int &h) const;

	// Framebuffer teardown (used on exit from OSystem_Arca::quit)
	void closeFramebuffer();

	// Blank screen (used on exit)
	void blankScreen();
};

#endif
