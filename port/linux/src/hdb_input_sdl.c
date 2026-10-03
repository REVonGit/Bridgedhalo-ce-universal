/*
HDB_INPUT_SDL.C

Routes the keyboard and mouse to UZDoom while Doom drives the player
(hdb_bridge.c). sdl_platform.c's event loop asks first; an event that went
to Doom never reaches input_state, so Halo's own movement, fire, grenades,
melee and weapon switch go quiet without touching the game. The keys in
hdbridge.ini's sHaloKeys (Esc, E, `, F1, F11, F12) always stay with Halo:
the pause menu, the action prompts and vehicles. Gamepads are not routed.
*/

#include "hdb_hooks.h"

#ifdef HALO_HDBRIDGE

/* the Xbox SDK declarations first, as every platform unit has them: on
Windows they keep SDL and gl.h from bringing in the Windows SDK */
#include "platform.h"
#include <SDL3/SDL.h>
#include "hdb_bridge.h"

/* SDL scancodes are USB HID usages; UZDoom's key codes are DirectInput
   scancodes (E0-extended keys | 0x80). */
static uint16_t sdl_to_dik(SDL_Scancode sc) {
    static const uint8_t letters[26] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
    };
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return letters[sc - SDL_SCANCODE_A];
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) return (uint16_t)(0x02 + (sc - SDL_SCANCODE_1));
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10) return (uint16_t)(0x3B + (sc - SDL_SCANCODE_F1));
    switch (sc) {
    case SDL_SCANCODE_RETURN:       return 0x1C;
    case SDL_SCANCODE_ESCAPE:       return 0x01;
    case SDL_SCANCODE_BACKSPACE:    return 0x0E;
    case SDL_SCANCODE_TAB:          return 0x0F;
    case SDL_SCANCODE_SPACE:        return 0x39;
    case SDL_SCANCODE_MINUS:        return 0x0C;
    case SDL_SCANCODE_EQUALS:       return 0x0D;
    case SDL_SCANCODE_LEFTBRACKET:  return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_BACKSLASH:    return 0x2B;
    case SDL_SCANCODE_SEMICOLON:    return 0x27;
    case SDL_SCANCODE_APOSTROPHE:   return 0x28;
    case SDL_SCANCODE_GRAVE:        return 0x29;
    case SDL_SCANCODE_COMMA:        return 0x33;
    case SDL_SCANCODE_PERIOD:       return 0x34;
    case SDL_SCANCODE_SLASH:        return 0x35;
    case SDL_SCANCODE_CAPSLOCK:     return 0x3A;
    case SDL_SCANCODE_F11:          return 0x57;
    case SDL_SCANCODE_F12:          return 0x58;
    case SDL_SCANCODE_SCROLLLOCK:   return 0x46;
    case SDL_SCANCODE_PAUSE:        return 0xC5;
    case SDL_SCANCODE_INSERT:       return 0xD2;
    case SDL_SCANCODE_HOME:         return 0xC7;
    case SDL_SCANCODE_PAGEUP:       return 0xC9;
    case SDL_SCANCODE_DELETE:       return 0xD3;
    case SDL_SCANCODE_END:          return 0xCF;
    case SDL_SCANCODE_PAGEDOWN:     return 0xD1;
    case SDL_SCANCODE_RIGHT:        return 0xCD;
    case SDL_SCANCODE_LEFT:         return 0xCB;
    case SDL_SCANCODE_DOWN:         return 0xD0;
    case SDL_SCANCODE_UP:           return 0xC8;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
    case SDL_SCANCODE_KP_DIVIDE:    return 0xB5;
    case SDL_SCANCODE_KP_MULTIPLY:  return 0x37;
    case SDL_SCANCODE_KP_MINUS:     return 0x4A;
    case SDL_SCANCODE_KP_PLUS:      return 0x4E;
    case SDL_SCANCODE_KP_ENTER:     return 0x9C;
    case SDL_SCANCODE_KP_1:         return 0x4F;
    case SDL_SCANCODE_KP_2:         return 0x50;
    case SDL_SCANCODE_KP_3:         return 0x51;
    case SDL_SCANCODE_KP_4:         return 0x4B;
    case SDL_SCANCODE_KP_5:         return 0x4C;
    case SDL_SCANCODE_KP_6:         return 0x4D;
    case SDL_SCANCODE_KP_7:         return 0x47;
    case SDL_SCANCODE_KP_8:         return 0x48;
    case SDL_SCANCODE_KP_9:         return 0x49;
    case SDL_SCANCODE_KP_0:         return 0x52;
    case SDL_SCANCODE_KP_PERIOD:    return 0x53;
    case SDL_SCANCODE_LCTRL:        return 0x1D;
    case SDL_SCANCODE_LSHIFT:       return 0x2A;
    case SDL_SCANCODE_LALT:         return 0x38;
    case SDL_SCANCODE_LGUI:         return 0xDB;
    case SDL_SCANCODE_RCTRL:        return 0x9D;
    case SDL_SCANCODE_RSHIFT:       return 0x36;
    case SDL_SCANCODE_RALT:         return 0xB8;
    case SDL_SCANCODE_RGUI:         return 0xDC;
    default:                        return 0;
    }
}

/* SDL buttons -> UZDoom KEY_MOUSE1 + n (left, right, middle, X1, X2). */
static int sdl_button_to_doom(Uint8 b) {
    switch (b) {
    case SDL_BUTTON_LEFT:   return 0;
    case SDL_BUTTON_RIGHT:  return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_X1:     return 3;
    case SDL_BUTTON_X2:     return 4;
    default:                return -1;
    }
}

int hdb_input_filter_sdl(void const *sdl_event)
{
	SDL_Event const *event = (SDL_Event const *)sdl_event;
	static float accumulated_x, accumulated_y, accumulated_wheel;

	switch (event->type)
	{
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP:
	{
		uint16_t dik;

		if (event->key.repeat)
			return hdb_bridge_driving();   /* Doom repeats keys itself */
		dik = sdl_to_dik(event->key.scancode);
		if (!dik)
			return 0;
		return hdb_bridge_key(dik, event->type == SDL_EVENT_KEY_DOWN);
	}
	case SDL_EVENT_MOUSE_MOTION:
	{
		int dx, dy;

		if (!hdb_bridge_driving())
		{
			accumulated_x = accumulated_y = 0.f;
			return 0;
		}
		/* sub-pixel motion adds up rather than being dropped */
		accumulated_x += event->motion.xrel;
		accumulated_y += event->motion.yrel;
		dx = (int)accumulated_x;
		dy = (int)accumulated_y;
		accumulated_x -= (float)dx;
		accumulated_y -= (float)dy;
		/* raw-input counts, +y = moved down: what UZDoom's PostMouseMove takes */
		return hdb_bridge_mouse_move(dx, dy);
	}
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
	{
		int button = sdl_button_to_doom(event->button.button);

		if (button < 0)
			return hdb_bridge_driving();
		return hdb_bridge_mouse_button((uint8_t)button, event->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
	}
	case SDL_EVENT_MOUSE_WHEEL:
	{
		int notches;

		if (!hdb_bridge_driving())
		{
			accumulated_wheel = 0.f;
			return 0;
		}
		accumulated_wheel += event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event->wheel.y : event->wheel.y;
		notches = (int)accumulated_wheel;
		accumulated_wheel -= (float)notches;
		if (notches)
			hdb_bridge_mouse_wheel(notches);
		return 1;
	}
	default:
		return 0;
	}
}

#endif /* HALO_HDBRIDGE */
