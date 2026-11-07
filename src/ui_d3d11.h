// Direct3D 11 drawing for the multiplayer menu, lobby and banners (ui.cpp), for a game that renders with
// Direct3D 11 (public_beta-26494568 with the graphics option DirectX 11, its default). ui.cpp draws the same
// rectangles and text with Direct3D 9 otherwise; what is drawn here looks the same: screen pixels, D3DCOLOR
// colours, alpha blending, text images from GDI.
#pragma once
#include "common.h"
#include <unknwn.h>

// Starts a frame: finds the engine's swap chain (and through it the device and its immediate context) in the
// i_render object and the size of the back buffer the frame goes to. 1: draw now; 0: not this frame;
// -1: Direct3D 11 drawing cannot work in this game (the log says why, once).
int ui11_begin(Object *render, float *w, float *h);
// Queued in order, drawn by ui11_flush. color: D3DCOLOR (0xAARRGGBB).
void ui11_rect(float x, float y, float w, float h, DWORD color);
// A texture of ui11_texture: the colour's rgb, the texture's alpha times the colour's (as the Direct3D 9 text).
void ui11_image(IUnknown *tex, float x, float y, float w, float h, DWORD color);
// An image of w x h pixels laid out as Direct3D 9's A8R8G8B8 (one DWORD 0xAARRGGBB a pixel, rows of w);
// 0 when it cannot be made. Released with Release().
IUnknown *ui11_texture(int w, int h, const DWORD *argb);
const void *ui11_owner(void);           // the device the textures belong to (another one: they are of no use)
void ui11_flush(void);                  // draws what is queued onto the back buffer; the engine's state is kept
// The back buffer as the tests' screenshot: half size, 24-bit BMP. Between ui11_flush and ui11_end.
bool ui11_save_frame(const char *path, unsigned *w, unsigned *h, unsigned *format);
void ui11_end(void);                    // ends the frame: lets go of the back buffer
void ui11_idle(void);                   // nothing is shown: lets go of every Direct3D 11 object made here
