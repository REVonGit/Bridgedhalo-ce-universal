/*
HDB_BRIDGE.H

The HaloDoom bridge's platform side: its settings (hdbridge.ini) and the
operating-system services the core (hdb_bridge.c) needs, implemented by
posix_hdbridge.c (Linux) and port/windows/src/win32_hdbridge.c. The game's
calls into the bridge are in port/linux/include/hdb_hooks.h.
*/

#ifndef __HDB_BRIDGE_H
#define __HDB_BRIDGE_H

#include <stdint.h>
#include "hdb_protocol.h"

#define HDB_PATH_MAX 512
#define HDB_MAX_RESERVED_KEYS 16
#define HDB_MAX_DAMAGE_TYPES 32

typedef struct
{
	/* [Doom] */
	int start_doom;
	int doom_visible;
	char uzdoom_exe[HDB_PATH_MAX];
	char iwad[HDB_PATH_MAX];
	char halodoom_pk3[HDB_PATH_MAX];
	char bridge_pk3[HDB_PATH_MAX];
	char doom_config[HDB_PATH_MAX];
	char extra_args[HDB_PATH_MAX];
	/* [Scale] */
	float doom_units_per_wu;
	float outgoing_damage_scale;
	float incoming_damage_scale;
	float proxy_radius_wu;
	/* [Overlay] */
	uint32_t key_rgb;
	uint32_t key_tolerance;
	/* [Input] DirectInput-style scancodes that stay with Halo */
	uint16_t halo_keys[HDB_MAX_RESERVED_KEYS];
	int halo_key_count;
	uint16_t both_keys[HDB_MAX_RESERVED_KEYS];   /* to Halo and Halo Doom alike */
	int both_key_count;
	/* [DamageTypes] Doom damage type name -> Halo damage_effect tag path */
	char damage_type_name[HDB_MAX_DAMAGE_TYPES][64];
	char damage_effect_path[HDB_MAX_DAMAGE_TYPES][128];
	int damage_type_count;
	char default_damage_effect_path[128];
} hdb_config;

/* operating system (posix_hdbridge.c, win32_hdbridge.c) */
int hdb_os_load_config(hdb_config *config);          /* hdbridge.ini next to the executable */
hdb_shared *hdb_os_map_shared(int *created_new);     /* 0 on failure */
void hdb_os_unmap_shared(hdb_shared *shared);
int hdb_os_process_alive(uint32_t pid);
int hdb_os_launch_doom(hdb_config const *config, uint32_t view_width, uint32_t view_height);
void hdb_os_kill_doom(void);
int hdb_os_doom_exited(long *exit_code);           /* 1 once the UZDoom it started has quit */
uint32_t hdb_os_pid(void);
uint64_t hdb_os_ms(void);
void hdb_os_log(char const *format, ...);
void hdb_os_file_path(char const *name, char *path, size_t size);   /* beside hdbridge.log */
int hdb_bridge_overlay_hidden(void);                              /* F10 */

/* the core, for the overlay and the input filter (hdb_bridge.c) */
hdb_shared *hdb_bridge_shared(void);
int hdb_bridge_driving(void);
void hdb_bridge_set_picture_size(int width, int height);
int hdb_bridge_key(uint16_t dik_scancode, int down);
int hdb_bridge_mouse_move(int32_t dx, int32_t dy);
int hdb_bridge_mouse_button(uint8_t doom_button, int down);
int hdb_bridge_mouse_wheel(int32_t notches);

#endif /* __HDB_BRIDGE_H */
