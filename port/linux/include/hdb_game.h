/*
HDB_GAME.H

What the bridge core (port/linux/src/hdb_bridge.c, a platform unit) asks of
the game (port/linux/game/hdb_game.c, a game unit). Plain C types only:
positions in world units, angles in radians, object and tag indices as the
game's own long datum indices (NONE = -1).
*/

#ifndef __HDB_GAME_H
#define __HDB_GAME_H

/* the same layout as hdb_vec3 in hdb_protocol.h */
typedef struct { float x, y, z; } hdb_game_vec3;

typedef struct
{
	long handle;
	hdb_game_vec3 pos;        /* feet */
	hdb_game_vec3 vel;        /* world units per tick */
	float radius, height, yaw;
	long team;
	unsigned long kind_hash;  /* FNV-1a of the lower-cased tag path */
	float health_frac;        /* (body + shield) / maximum, 0..1 */
	int alive;
	int is_vehicle;
} hdb_unit_info;

typedef struct
{
	int hit;
	long entity;              /* -1 for the structure BSP */
	hdb_game_vec3 point, normal;
} hdb_ray_hit;

/* a weapon or equipment lying loose in the world */
typedef struct
{
	long handle;
	hdb_game_vec3 pos;
	unsigned long kind_hash;  /* FNV-1a of the lower-cased tag path */
	float count;              /* a weapon's rounds (or charge, 0..100); 1 otherwise */
	int is_weapon;
} hdb_item_info;

typedef void (*hdb_unit_fn)(long handle, void *context);

/* game state */
int hdb_game_in_progress(void);       /* a campaign map runs and local player 0 has a unit */
int hdb_game_paused(void);            /* pause menu, console, main menu */
int hdb_game_menu_open(void);         /* main menu or console (not a paused game clock) */
void hdb_game_set_time_paused(int paused);   /* freezes the world, as Halo's pause does */
int hdb_game_cinematic(void);
char const *hdb_game_map_name(void);  /* e.g. levels\a10\a10, or 0 */
long hdb_game_bsp_index(void);
int hdb_game_bsp_bounds(hdb_game_vec3 *minimum, hdb_game_vec3 *maximum);
float hdb_game_fov_horizontal(void);  /* of the picture drawn, radians */

/* local player 0 */
long hdb_game_player_unit(void);      /* -1 if none */
int hdb_game_player_in_vehicle(void);
int hdb_game_player_dead(void);
int hdb_game_player_grounded(void);
int hdb_game_player_in_water(void);
int hdb_game_get_unit(long handle, hdb_unit_info *info);
void hdb_game_for_each_unit(hdb_unit_fn fn, void *context);
int hdb_game_get_item(long handle, hdb_item_info *info);   /* 0 if gone or carried */
void hdb_game_for_each_item(hdb_unit_fn fn, void *context);
int hdb_game_delete_item(long handle);                     /* 0 if gone or carried */

/* presentation */
void hdb_game_set_fp_weapon_and_hud_visible(int visible);

/* world */
int hdb_game_ray_test(hdb_game_vec3 const *from, hdb_game_vec3 const *to, int include_objects,
	long ignore_object, hdb_ray_hit *hit);
long hdb_game_damage_effect(char const *tag_path);
char const *hdb_game_tag_name(long tag_index);        /* "?" for none */
void hdb_game_damage_object(long target, float amount, long damage_effect,
	hdb_game_vec3 const *origin, hdb_game_vec3 const *direction);
void hdb_game_kill_player(void);

#endif /* __HDB_GAME_H */
