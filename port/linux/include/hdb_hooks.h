/*
HDB_HOOKS.H

The HaloDoom bridge (port/linux/src/hdb_*.c): Halo Doom Evolved, running in
a hidden UZDoom, drives the player's biped through shared memory, the way
SkyCraft drives Skyrim with Minecraft. These are the calls the game makes
into it, in plain C types so game units (which do not see <stdint.h>) and
platform units can both include this header.

Everything is inert without hdbridge.ini next to the executable: the bridge
then never starts, and every override below answers "not driving".
*/

#ifndef __HDB_HOOKS_H
#define __HDB_HOOKS_H

/* the desktop ports (Windows, Linux); not Android */
#ifndef HALO_ANDROID
#define HALO_HDBRIDGE 1
#endif

#ifdef HALO_HDBRIDGE

/* sdl_platform.c, once the window and GL context exist / at exit */
void hdb_bridge_init(void);
void hdb_bridge_shutdown(void);

/* main.c, every frame (the pause menu runs between ticks): who drives,
the state UZDoom mirrors, Doom's aim */
void hdb_bridge_frame(void);
/* game.c, first thing in game_tick: Doom's movement, damage and ray
queries for this tick, before units_update moves the bipeds */
void hdb_bridge_tick(void);

/* player_control.c: when it returns 1 for local player 0, the player
faces *yaw / *pitch (radians, Halo's convention), crouches when *crouch,
and *forward / *left (-1..1) is the throttle that animates the biped */
int hdb_bridge_aim_override(float *yaw, float *pitch, int *crouch, float *forward, float *left);

/* bipeds.c, biped_update_physics: when it returns 1 for this biped, its
velocity this tick is velocity[3] (world units per tick); collision is
still Halo's */
int hdb_bridge_motion_override(long biped_index, float velocity[3], float halo_gravity);

/* damage.c, object_cause_damage: returns 1 when the victim is the player's
unit and the damage went to Doom instead (Doom owns the player's shields
and health) */
int hdb_bridge_player_damaged(long victim_index, float amount, float const source[3], long damage_effect_index);

/* game_save / game_revert */
void hdb_bridge_checkpoint_saved(void);
void hdb_bridge_reverted(void);

/* the first-person weapon and the HUD: hidden while Doom draws its own */
int hdb_game_fp_weapon_and_hud_visible(void);

/* sdl_platform.c's event loop: 1 when the event went to Doom (the platform
must not turn it into controller 1's state). Takes a const SDL_Event *. */
int hdb_input_filter_sdl(void const *sdl_event);

/* d3d8_gl.c, D3DDevice_Present before the swap: the picture's rectangle in
GL window coordinates (origin bottom-left) */
void hdb_overlay_draw(int x, int y, int width, int height);

#endif /* HALO_HDBRIDGE */

#endif /* __HDB_HOOKS_H */
