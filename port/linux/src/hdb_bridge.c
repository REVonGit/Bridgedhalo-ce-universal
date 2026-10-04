/*
HDB_BRIDGE.C

The HaloDoom bridge's core. Halo keeps its world, AI, scripts, checkpoints
and rendering; a hidden UZDoom running Halo Doom Evolved owns the player:
its movement, weapons, shields and HUD. The two meet in shared memory
(hdb_protocol.h).

Every frame (hdb_bridge_frame, from the main loop, which runs during the
pause menu too) it decides who drives the player, reads Doom's aim and
publishes the player, nearby units and the game's state for Doom to mirror.
Every tick (hdb_bridge_tick, before units_update) it takes the movement Doom
computed, turns Doom's hits into Halo damage after a line-of-sight check, and
answers Doom's ray queries against Halo's collision.

No operating system or game headers here: hdb_bridge.h for the OS,
hdb_game.h for the game.
*/

#include "hdb_hooks.h"

#ifdef HALO_HDBRIDGE

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "hdb_bridge.h"
#include "hdb_game.h"

#define HDB_MAX_CANDIDATES 1024
#define HDB_MAX_HELD 64

typedef struct
{
	float d2;
	hdb_unit_info unit;
} hdb_near;

static struct
{
	hdb_config config;
	hdb_shared *shared;
	int ready;

	uint32_t frame;
	uint32_t tick;
	int in_game;
	int driving;
	int killing;        /* our own kill of the biped must not go back to Doom */
	long player;

	uint32_t map_id;
	char last_map[64];
	long last_bsp;
	hdb_vec3 origin;
	long damage_effects[HDB_MAX_DAMAGE_TYPES];
	uint32_t damage_hashes[HDB_MAX_DAMAGE_TYPES];
	long default_damage_effect;
	long explosion_damage_effect;   /* any other explosion of Halo Doom's */

	uint32_t last_doom_heartbeat;
	uint64_t last_doom_change_ms;

	hdb_vec3 motion;    /* this tick's Doom displacement, world units */
	int have_motion;
	int hits_dealt, hits_walled, hits_lost;   /* Doom's hits, for the log every 10 s */
	int starved;
	float yaw, pitch;
	int crouch;

	uint32_t picture_width, picture_height;
	int have_picture;
	int launch_pending;
	int doom_seen;   /* 0 nothing yet, 1 talking, 2 ready: for the log */
	int doom_menu;        /* UZDoom's menu or console is open while it drives */
	float sent_shields, sent_body;   /* the player's vitality as Doom last heard it (-1: resend) */
	int player_was_dead;
	char loadout_map[64];            /* the level whose starting loadout Doom has */
	float doom_zoom;                 /* Halo Doom's view magnification (1: none) */
	struct { long unit; uint32_t until_tick; } stuck[16];   /* units with a Doom grenade stuck to them */
	int paused_for_doom;  /* we stopped Halo's clock for it */
	uint64_t init_ms;

	volatile long input_lock;
	uint16_t held[HDB_MAX_HELD];
	int held_count;
} B;

static hdb_near near_units[HDB_MAX_CANDIDATES];
static int near_count;
static hdb_vec3 near_centre;

/* loose weapons and equipment within this of the player go to Doom as items
to pick up (Halo's own pickups are off while Doom drives: players.c) */
#define HDB_ITEM_REACH_WU 4.f
#define HDB_MAX_ITEMS 32
static hdb_item_info near_items[HDB_MAX_ITEMS];
static int near_item_count;

/* ---------- helpers */

static hdb_vec3 to_vec3(hdb_game_vec3 v) { hdb_vec3 r; r.x = v.x; r.y = v.y; r.z = v.z; return r; }
static hdb_game_vec3 to_game_vec3(hdb_vec3 v) { hdb_game_vec3 r; r.x = v.x; r.y = v.y; r.z = v.z; return r; }

static float distance2(hdb_vec3 a, hdb_vec3 b)
{
	float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
	return x * x + y * y + z * z;
}

static int held_find(uint16_t key)
{
	int i;
	for (i = 0; i < B.held_count; i++)
		if (B.held[i] == key)
			return i;
	return -1;
}

static void held_add(uint16_t key)
{
	if (held_find(key) < 0 && B.held_count < HDB_MAX_HELD)
		B.held[B.held_count++] = key;
}

static int held_remove(uint16_t key)
{
	int i = held_find(key);
	if (i < 0)
		return 0;
	B.held[i] = B.held[--B.held_count];
	return 1;
}

/* the input ring has one consumer but two producers here: the event loop
(keys, mouse) and the frame (release-all); they are serialised */
static void input_lock(void)
{
	while (__atomic_exchange_n(&B.input_lock, 1, __ATOMIC_ACQUIRE))
		;
}

static void input_unlock(void)
{
	__atomic_store_n(&B.input_lock, 0, __ATOMIC_RELEASE);
}

static int push_input(uint16_t type, uint16_t code, int32_t dx, int32_t dy)
{
	hdb_input in;
	int ok;

	in.type = type;
	in.code = code;
	in.dx = dx;
	in.dy = dy;
	input_lock();
	HDB_RING_PUSH(B.shared->input, in, ok);
	input_unlock();
	return ok;
}

static void push_halo_event(uint32_t type, float amount, uint32_t damage_type, hdb_vec3 const *source)
{
	hdb_event event;
	int ok;

	memset(&event, 0, sizeof(event));
	event.type = type;
	event.amount = amount;
	event.dtype_hash = damage_type;
	if (source)
		event.source = *source;
	HDB_RING_PUSH(B.shared->halo_events, event, ok);
	(void)ok;
}

/* ---------- lifetime */

void hdb_bridge_init(void)
{
	int created = 0;
	uint32_t attached_doom = 0;

	memset(&B, 0, sizeof(B));
	B.last_bsp = -1;
	B.player = -1;
	B.picture_width = 854;
	B.picture_height = 480;

	if (!hdb_os_load_config(&B.config))
		return;   /* no hdbridge.ini: Halo plays as ever */
	B.shared = hdb_os_map_shared(&created);
	if (!B.shared)
	{
		hdb_os_log("could not map the bridge memory: bridge off");
		return;
	}
	if (!created && B.shared->magic == HDB_MAGIC && B.shared->version == HDB_PROTOCOL_VERSION)
		attached_doom = B.shared->doom_pid;   /* a UZDoom from an earlier run may still be attached */
	memset((void *)B.shared, 0, sizeof(hdb_shared));

	B.shared->magic = HDB_MAGIC;
	B.shared->version = HDB_PROTOCOL_VERSION;
	B.shared->size = (uint32_t)sizeof(hdb_shared);
	B.shared->halo_pid = hdb_os_pid();
	B.shared->doom_units_per_wu = B.config.doom_units_per_wu;
	B.shared->overlay_key_rgb = B.config.key_rgb;
	B.shared->overlay_key_tolerance = B.config.key_tolerance;
	B.shared->overlay.front = HDB_NONE;
	B.shared->overlay.reading = HDB_NONE;
	B.ready = 1;
	B.init_ms = hdb_os_ms();
	hdb_os_log("bridge memory ready (%u bytes, protocol %u)", (unsigned)sizeof(hdb_shared), HDB_PROTOCOL_VERSION);

	if (B.config.start_doom)
	{
		if (attached_doom && hdb_os_process_alive(attached_doom))
		{
			/* never a second UZDoom: this one re-attaches by itself */
			B.shared->doom_pid = attached_doom;
			hdb_os_log("UZDoom already running (pid %u)", attached_doom);
		}
		else
		{
			B.launch_pending = 1;   /* from the frame, at the picture's real size */
		}
	}
}

void hdb_bridge_shutdown(void)
{
	if (!B.ready)
		return;
	if (B.driving)
		hdb_game_set_fp_weapon_and_hud_visible(1);
	if (B.paused_for_doom)
		hdb_game_set_time_paused(0);
	hdb_os_kill_doom();
	hdb_os_unmap_shared(B.shared);
	B.shared = NULL;
	B.ready = 0;
}

hdb_shared *hdb_bridge_shared(void) { return B.ready ? B.shared : NULL; }
int hdb_bridge_driving(void) { return B.ready && B.driving; }

void hdb_bridge_set_picture_size(int width, int height)
{
	if (width > 0 && height > 0)
	{
		B.picture_width = (uint32_t)width;
		B.picture_height = (uint32_t)height;
		B.have_picture = 1;
	}
}

/* UZDoom renders at the size it starts with: wait for the first presented
frame to say the picture's size (or three seconds) */
static void maybe_launch_doom(void)
{
	uint32_t width = B.picture_width, height = B.picture_height;

	if (!B.launch_pending)
		return;
	if (!B.have_picture && hdb_os_ms() - B.init_ms < 3000)
		return;
	B.launch_pending = 0;
	/* the overlay's size limit, keeping the picture's shape */
	if (width > HDB_OVERLAY_MAX_W || height > HDB_OVERLAY_MAX_H)
	{
		float scale = fminf((float)HDB_OVERLAY_MAX_W / width, (float)HDB_OVERLAY_MAX_H / height);
		width = (uint32_t)(width * scale) & ~1u;
		height = (uint32_t)(height * scale) & ~1u;
	}
	hdb_os_launch_doom(&B.config, width, height);
}

/* ---------- every frame */

static int doom_alive(void)
{
	uint64_t now = hdb_os_ms();

	if (B.shared->doom_heartbeat != B.last_doom_heartbeat)
	{
		B.last_doom_heartbeat = B.shared->doom_heartbeat;
		B.last_doom_change_ms = now;
	}
	return B.last_doom_change_ms && now - B.last_doom_change_ms < 1500;
}

static void set_driving(int on)
{
	if (on == B.driving)
		return;
	B.driving = on;
	B.sent_shields = B.sent_body = -1.f;
	B.doom_zoom = 1.f;
	hdb_game_set_fp_weapon_and_hud_visible(!on);
	if (!on)
	{
		push_input(HDB_IN_RELEASE_ALL, 0, 0, 0);
		B.held_count = 0;
		B.have_motion = 0;
	}
	hdb_os_log(on ? "Doom drives the player" : "Halo has the player back");
}

static void resolve_damage_effects(void)
{
	int i;

	B.default_damage_effect = B.config.default_damage_effect_path[0]
		? hdb_game_damage_effect(B.config.default_damage_effect_path) : -1;
	if (B.config.default_damage_effect_path[0] && B.default_damage_effect == -1)
		hdb_os_log("damage effect not in this map: %s", B.config.default_damage_effect_path);
	{
		/* Halo's explosions this map has, for Halo Doom's other ones */
		static char const *const explosions[] =
		{
			"weapons\\frag grenade\\explosion",
			"weapons\\plasma grenade\\explosion",
			"weapons\\rocket launcher\\explosion",
		};
		B.explosion_damage_effect = -1;
		for (i = 0; i < (int)(sizeof(explosions) / sizeof(explosions[0])) && B.explosion_damage_effect == -1; i++)
			B.explosion_damage_effect = hdb_game_damage_effect_exact(explosions[i]);
	}
	for (i = 0; i < B.config.damage_type_count; i++)
	{
		B.damage_hashes[i] = hdb_fnv1a_lower(B.config.damage_type_name[i]);
		/* a type whose tag this map lacks falls back (effect_for) */
		B.damage_effects[i] = hdb_game_damage_effect_exact(B.config.damage_effect_path[i]);
	}
}

static void check_map_change(void)
{
	char const *name = hdb_game_map_name();
	long bsp = hdb_game_bsp_index();
	hdb_game_vec3 minimum, maximum;

	if (!name)
		return;
	if (strcmp(B.last_map, name) == 0 && B.last_bsp == bsp)
		return;
	strncpy(B.last_map, name, sizeof(B.last_map) - 1);
	B.last_bsp = bsp;
	B.map_id++;
	/* recentred so Doom's coordinates stay inside its void map */
	if (hdb_game_bsp_bounds(&minimum, &maximum))
	{
		B.origin.x = (minimum.x + maximum.x) * 0.5f;
		B.origin.y = (minimum.y + maximum.y) * 0.5f;
		B.origin.z = (minimum.z + maximum.z) * 0.5f;
	}
	else
	{
		memset(&B.origin, 0, sizeof(B.origin));
	}
	resolve_damage_effects();   /* tag indices differ from map to map */
	push_halo_event(HDB_EV_MAP_LOADED, 0.f, 0, NULL);
	hdb_os_log("map %s bsp %ld: map %u", name, bsp, B.map_id);
}

static void collect_unit(long handle, void *context)
{
	hdb_unit_info unit;
	float d2, r2 = B.config.proxy_radius_wu * B.config.proxy_radius_wu;

	(void)context;
	if (handle == B.player || near_count >= HDB_MAX_CANDIDATES)
		return;
	if (!hdb_game_get_unit(handle, &unit) || !unit.alive)
		return;
	d2 = distance2(to_vec3(unit.pos), near_centre);
	if (d2 > r2)
		return;
	near_units[near_count].d2 = d2;
	near_units[near_count].unit = unit;
	near_count++;
}

static void collect_item(long handle, void *context)
{
	hdb_item_info item;

	(void)context;
	if (near_item_count >= HDB_MAX_ITEMS || !hdb_game_get_item(handle, &item))
		return;
	if (distance2(to_vec3(item.pos), near_centre) > HDB_ITEM_REACH_WU * HDB_ITEM_REACH_WU)
		return;
	near_items[near_item_count++] = item;
}

static int by_distance(void const *a, void const *b)
{
	float x = ((hdb_near const *)a)->d2, y = ((hdb_near const *)b)->d2;
	return (x > y) - (x < y);
}

static void publish_state(uint32_t flags)
{
	hdb_unit_info me;
	hdb_halo_state *s = &B.shared->halo;
	int have_me, i;
	float ground;

	memset(&me, 0, sizeof(me));
	have_me = B.player != -1 && hdb_game_get_unit(B.player, &me);

	near_count = 0;
	near_item_count = 0;
	if (have_me)
	{
		near_centre = to_vec3(me.pos);
		hdb_game_for_each_unit(collect_unit, NULL);
		if (near_count > HDB_MAX_PROXIES - HDB_MAX_ITEMS)
		{
			qsort(near_units, (size_t)near_count, sizeof(near_units[0]), by_distance);
			near_count = HDB_MAX_PROXIES - HDB_MAX_ITEMS;
		}
		if (B.driving)
			hdb_game_for_each_item(collect_item, NULL);
	}

	ground = me.pos.z;
	if (have_me)
	{
		hdb_game_vec3 from = me.pos, to = me.pos;
		hdb_ray_hit hit;

		from.z += 0.05f;
		to.z -= 2.0f;
		if (hdb_game_ray_test(&from, &to, 1, B.player, &hit) && hit.hit)
			ground = hit.point.z;
	}

	HDB_SEQ_BEGIN(*s);
	s->halo_tick = B.tick;
	s->flags = flags;
	s->map_id = B.map_id;
	memset(s->map_name, 0, sizeof(s->map_name));
	strncpy(s->map_name, B.last_map, sizeof(s->map_name) - 1);
	s->world_origin = B.origin;
	s->player_pos = to_vec3(me.pos);
	s->player_vel = to_vec3(me.vel);
	s->ground_z = ground;
	s->fov_h = hdb_game_fov_horizontal();
	s->view_w = B.picture_width;
	s->view_h = B.picture_height;
	s->proxy_count = (uint32_t)(near_count + near_item_count);
	for (i = 0; i < near_count; i++)
	{
		hdb_unit_info const *u = &near_units[i].unit;
		hdb_proxy *p = &s->proxies[i];

		p->entity_id = (uint32_t)u->handle;
		p->flags = HDB_PF_ALIVE |
			(have_me && u->team != me.team ? (uint32_t)HDB_PF_ENEMY : 0u) |
			(u->is_vehicle ? (uint32_t)HDB_PF_VEHICLE : 0u);
		p->pos = to_vec3(u->pos);
		p->vel = to_vec3(u->vel);
		p->radius = u->radius;
		p->height = u->height;
		p->yaw = u->yaw;
		p->team = (uint32_t)u->team;
		p->kind_hash = (uint32_t)u->kind_hash;
		p->health_frac = u->health_frac;
	}
	for (i = 0; i < near_item_count; i++)
	{
		hdb_item_info const *item = &near_items[i];
		hdb_proxy *p = &s->proxies[near_count + i];

		memset(p, 0, sizeof(*p));
		p->entity_id = (uint32_t)item->handle;
		p->flags = HDB_PF_ITEM;
		p->pos = to_vec3(item->pos);
		p->radius = 0.15f;
		p->height = 0.15f;
		p->team = HDB_NONE;
		p->kind_hash = (uint32_t)item->kind_hash;
		p->health_frac = item->count;
	}
	HDB_SEQ_END(*s);
}

void hdb_bridge_frame(void)
{
	uint32_t flags = 0;
	int doom_up, drive;
	hdb_doom_state doom;

	if (!B.ready)
		return;
	B.shared->halo_heartbeat = ++B.frame;
	maybe_launch_doom();

	doom_up = doom_alive() && (B.shared->doom.flags & HDB_DS_READY);
	{
		/* what UZDoom is doing, for hdbridge.log */
		int seen = doom_up ? 2 : doom_alive() ? 1 : 0;
		long code;

		if (seen != B.doom_seen)
		{
			if (seen == 2) hdb_os_log("UZDoom ready");
			else if (seen == 1) hdb_os_log(B.doom_seen ? "UZDoom no longer ready" : "UZDoom connected");
			else hdb_os_log("UZDoom stopped responding");
			B.doom_seen = seen;
		}
		if ((B.frame & 31) == 0 && hdb_os_doom_exited(&code))
			hdb_os_log("UZDoom quit (exit code %ld%s)", code,
				(unsigned long)code == 0xC0000005UL ? ": a crash, see its crash report" : "");
	}
	B.in_game = hdb_game_in_progress();
	B.player = B.in_game ? hdb_game_player_unit() : -1;
	if (B.player == -1)
		B.in_game = 0;

	if (B.in_game)
	{
		flags |= HDB_HS_IN_GAME;
		check_map_change();
		if (hdb_game_player_dead()) flags |= HDB_HS_PLAYER_DEAD;
		if (hdb_game_player_in_vehicle()) flags |= HDB_HS_HALO_CONTROL;
		if (hdb_game_player_grounded()) flags |= HDB_HS_GROUNDED;
		if (hdb_game_player_in_water()) flags |= HDB_HS_IN_WATER;
	}
	else
	{
		flags |= HDB_HS_LOADING;
	}
	{
		/* UZDoom's menu or console (its own keys, 9 and 0 by default): Halo's
		world waits, with Doom still driving and drawn, so the menu shows */
		int doom_menu = doom_up && (B.shared->doom.flags & HDB_DS_MENU) != 0;

		if (doom_menu && B.driving && !B.paused_for_doom && !hdb_game_paused())
		{
			hdb_game_set_time_paused(1);
			B.paused_for_doom = 1;
		}
		else if (!doom_menu && B.paused_for_doom)
		{
			hdb_game_set_time_paused(0);
			B.paused_for_doom = 0;
		}
		B.doom_menu = doom_menu && B.paused_for_doom;
	}
	if (B.paused_for_doom ? hdb_game_menu_open() : hdb_game_paused()) flags |= HDB_HS_PAUSED;
	if (B.in_game && hdb_game_cinematic()) flags |= HDB_HS_CINEMATIC;

	/* UZDoom quitting or crashing hands the player straight back */
	drive = doom_up && B.in_game &&
		!(flags & (HDB_HS_PAUSED | HDB_HS_CINEMATIC | HDB_HS_PLAYER_DEAD | HDB_HS_HALO_CONTROL));
	set_driving(drive);
	if (drive)
		flags |= HDB_HS_DRIVING;

	/* Doom's aim, every frame, so turning is as smooth as Doom's */
	if (hdb_seq_read(&B.shared->doom, &doom, sizeof(doom), &B.shared->doom.seq))
	{
		B.yaw = doom.yaw;
		B.pitch = doom.pitch;
		B.crouch = (doom.flags & HDB_DS_CROUCHING) != 0;
	}

	publish_state(flags);
}

/* ---------- every tick */

static long effect_for(uint32_t damage_type, uint32_t flags)
{
	int i;

	for (i = 0; i < B.config.damage_type_count; i++)
		if (B.damage_hashes[i] == damage_type && B.damage_effects[i] != -1)
			return B.damage_effects[i];
	/* an explosion of another type: Halo's frag grenade's, for its push */
	if ((flags & HDB_DF_EXPLOSION) && B.explosion_damage_effect != -1)
		return B.explosion_damage_effect;
	return B.default_damage_effect;
}

/* The player's unit as Halo Doom shows it: shields and health (Halo's), its
death, and at a level's start what it carries */
static void send_loadout(void)
{
	hdb_carried_weapon weapons[4];
	int grenades[2], count, i;
	static char const *const grenade_tags[2] =
	{
		"weapons\\frag grenade\\frag grenade",
		"weapons\\plasma grenade\\plasma grenade",
	};

	count = hdb_game_player_loadout(weapons, 4, grenades);
	push_halo_event(HDB_EV_LOADOUT_BEGIN, 0.f, 0, NULL);
	for (i = 0; i < count; i++)
	{
		hdb_vec3 extra;

		extra.x = (float)weapons[i].loaded;
		extra.y = weapons[i].charge;
		extra.z = weapons[i].in_hand ? 1.f : 0.f;
		push_halo_event(HDB_EV_LOADOUT_ITEM, (float)weapons[i].reserve, (uint32_t)weapons[i].kind_hash, &extra);
	}
	for (i = 0; i < 2; i++)
	{
		if (grenades[i] > 0)
			push_halo_event(HDB_EV_LOADOUT_ITEM, (float)grenades[i], hdb_fnv1a_lower(grenade_tags[i]), NULL);
	}
	push_halo_event(HDB_EV_LOADOUT_END, 0.f, 0, NULL);
	hdb_os_log("%s: Doom starts with the player's %d weapons, %d frag and %d plasma grenades",
		B.last_map, count, grenades[0], grenades[1]);
}

static void sync_player(void)
{
	float shields, body;
	int dead;

	if (B.doom_seen != 2 || !B.in_game)
		return;
	dead = hdb_game_player_dead();
	if (dead && !B.player_was_dead)
	{
		hdb_os_log("the player died in Halo: so does Doom's");
		push_halo_event(HDB_EV_PLAYER_KILLED, 0.f, 0, NULL);
	}
	B.player_was_dead = dead;
	if (!B.driving)
		return;

	if (hdb_game_player_vitality(&shields, &body) &&
		(fabsf(shields - B.sent_shields) > 0.002f || fabsf(body - B.sent_body) > 0.002f))
	{
		hdb_vec3 v;

		v.x = shields;
		v.y = body;
		v.z = 0.f;
		push_halo_event(HDB_EV_PLAYER_VITALITY, 0.f, 0, &v);
		B.sent_shields = shields;
		B.sent_body = body;
	}

	if (strcmp(B.loadout_map, B.last_map) != 0)
	{
		send_loadout();
		strncpy(B.loadout_map, B.last_map, sizeof(B.loadout_map) - 1);
	}
}

void hdb_bridge_tick(void)
{
	hdb_move move;
	hdb_damage damage;
	hdb_ray_req request;
	hdb_event event;
	hdb_vec3 sum = { 0.f, 0.f, 0.f };
	int count = 0, ok;

	if (!B.ready)
		return;
	B.tick++;

	/* 35 Doom tics a second into 30 ticks: one each, sometimes two */
	for (;;)
	{
		HDB_RING_POP(B.shared->moves, move, ok);
		if (!ok)
			break;
		sum.x += move.delta.x;
		sum.y += move.delta.y;
		sum.z += move.delta.z;
		count++;
	}
	if (count)
	{
		B.motion = sum;
		B.have_motion = 1;
		B.starved = 0;
	}
	else if (++B.starved > 2)
	{
		memset(&B.motion, 0, sizeof(B.motion));   /* Doom hitched: stop rather than drift */
	}

	for (;;)
	{
		hdb_unit_info target;

		HDB_RING_POP(B.shared->damage, damage, ok);
		if (!ok)
			break;
		if (!B.driving || damage.target_id == HDB_NONE)
			continue;
		if (!hdb_game_get_unit((long)damage.target_id, &target) || !target.alive)
		{
			B.hits_lost++;
			continue;
		}
		if (damage.flags & HDB_DF_NEEDS_LOS)
		{
			/* Doom's void has no walls: Halo's structure between the shot
			and the target's middle decides */
			hdb_game_vec3 from = to_game_vec3(damage.origin), middle = target.pos;
			hdb_ray_hit hit;

			middle.z += target.height * 0.5f;
			if (hdb_game_ray_test(&from, &middle, 0, B.player, &hit) && hit.hit)
			{
				B.hits_walled++;
				continue;
			}
		}
		{
			hdb_game_vec3 origin = to_game_vec3(damage.origin), direction = to_game_vec3(damage.dir);

			hdb_game_damage_object((long)damage.target_id, damage.amount * B.config.outgoing_damage_scale,
				effect_for(damage.dtype_hash, damage.flags), &origin, &direction);
			B.hits_dealt++;
		}
	}

	sync_player();

	if (B.tick % 300 == 0 && (B.hits_dealt || B.hits_walled || B.hits_lost))
	{
		hdb_os_log("Doom's hits, last 10 s: %d dealt, %d stopped by walls, %d on units already gone",
			B.hits_dealt, B.hits_walled, B.hits_lost);
		B.hits_dealt = B.hits_walled = B.hits_lost = 0;
	}

	for (;;)
	{
		hdb_ray_hit hit;
		hdb_ray_result result;
		hdb_game_vec3 from, to;
		int pushed;

		HDB_RING_POP(B.shared->ray_requests, request, ok);
		if (!ok)
			break;
		from = to_game_vec3(request.from);
		to = to_game_vec3(request.to);
		memset(&hit, 0, sizeof(hit));
		hdb_game_ray_test(&from, &to, (request.flags & 1u) != 0, B.player, &hit);
		result.req_id = request.req_id;
		result.hit = hit.hit ? 1u : 0u;
		result.hit_entity = hit.hit && hit.entity != -1 ? (uint32_t)hit.entity : HDB_NONE;
		result.point = to_vec3(hit.point);
		result.normal = to_vec3(hit.normal);
		HDB_RING_PUSH(B.shared->ray_results, result, pushed);
		(void)pushed;
	}

	for (;;)
	{
		HDB_RING_POP(B.shared->doom_events, event, ok);
		if (!ok)
			break;
		if (event.type == HDB_EV_DOOM_ZOOM)
		{
			float zoom = (float)event.dtype_hash / 1000.f;
			B.doom_zoom = zoom < 1.f ? 1.f : zoom > 30.f ? 30.f : zoom;
			continue;
		}
		if (event.type == HDB_EV_DOOM_STUCK)
		{
			/* held a few ticks; Doom says it again every tic while stuck */
			int i, free_slot = -1;
			for (i = 0; i < 16; i++)
			{
				if (B.stuck[i].unit == (long)event.dtype_hash) break;
				if (free_slot < 0 && (B.stuck[i].until_tick < B.tick || !B.stuck[i].unit)) free_slot = i;
			}
			if (i == 16) i = free_slot;
			if (i >= 0)
			{
				B.stuck[i].unit = (long)event.dtype_hash;
				B.stuck[i].until_tick = B.tick + 3;
			}
			continue;
		}
		if (event.type == HDB_EV_DOOM_TOOK_ITEM)
		{
			/* Halo Doom picked up its version of it: Halo's goes */
			hdb_game_delete_item((long)event.dtype_hash);
			continue;
		}
		if (event.type == HDB_EV_DOOM_PLAYER_DIED && B.driving)
		{
			hdb_os_log("the Doom player died: so does the biped (Halo reverts as usual)");
			B.killing = 1;
			hdb_game_kill_player();
			B.killing = 0;
		}
	}
}

/* ---------- overrides the game consults */

int hdb_bridge_aim_override(float *yaw, float *pitch, int *crouch, float *forward, float *left)
{
	/* the biped's throttle from Doom's motion, for its animations and
	footsteps: world velocity turned into the facing's frame, about full
	deflection at a run (0.07 world units a tick) */
	const float run = 0.07f;
	float c, s, f, l;

	if (!B.ready || !B.driving)
		return 0;
	c = cosf(B.yaw);
	s = sinf(B.yaw);
	f = (B.motion.x * c + B.motion.y * s) / run;
	l = (-B.motion.x * s + B.motion.y * c) / run;
	*yaw = B.yaw;
	*pitch = B.pitch;
	*crouch = B.crouch;
	*forward = f > 1.f ? 1.f : f < -1.f ? -1.f : f;
	*left = l > 1.f ? 1.f : l < -1.f ? -1.f : l;
	return 1;
}

int hdb_bridge_motion_override(long biped_index, float velocity[3])
{
	if (!B.ready || !B.driving || !B.have_motion || biped_index != B.player || biped_index == -1)
		return 0;
	velocity[0] = B.motion.x;
	velocity[1] = B.motion.y;
	velocity[2] = B.motion.z;
	return 1;
}

float hdb_bridge_zoom(void)
{
	/* Halo Doom's zoom (its scope), for Halo's first-person camera */
	return B.ready && B.driving && B.doom_zoom > 1.f ? B.doom_zoom : 1.f;
}

long hdb_bridge_stuck_grenade_source(long unit_index)
{
	/* a Halo Doom grenade stuck to this unit: the AI reacts as to a Halo
	one stuck to it (ai/actors.c), the player being the one who threw it */
	int i;

	if (!B.ready || B.player == -1)
		return -1;
	for (i = 0; i < 16; i++)
		if (B.stuck[i].unit == unit_index && B.stuck[i].until_tick >= B.tick)
			return B.player;
	return -1;
}

int hdb_bridge_pickups_are_dooms(void)
{
	/* Halo Doom picks up Halo's weapons and equipment in its own way (the
	items go to it as proxies): Halo's player takes none itself */
	return B.ready && B.driving;
}

int hdb_bridge_no_falling_damage(long biped_index)
{
	/* Halo Doom has no falling damage: a landing hurts nobody Doom drives
	(falling out of the level still kills, by its own damage) */
	return B.ready && B.driving && biped_index == B.player && biped_index != -1;
}

int hdb_bridge_player_damaged(long victim_index, float amount, float const source[3], long damage_effect_index,
	int kill_instantly)
{
	/* Halo deals all damage to the player's unit, by its own rules (the
	weapon's shield and body multipliers, materials, head shots, shield leak
	and the difficulty); Halo Doom shows the result (the vitality, every
	tick) and gets each hit for its effects: shield sounds, the flash, which
	way it came from. Its death follows the unit's. */
	hdb_vec3 from;

	(void)kill_instantly;
	if (!B.ready || !B.driving || victim_index != B.player || victim_index == -1)
		return 0;
	from.x = source[0];
	from.y = source[1];
	from.z = source[2];
	push_halo_event(HDB_EV_PLAYER_DAMAGED, amount, (uint32_t)damage_effect_index, &from);
	return 0;
}

void hdb_bridge_checkpoint_saved(void)
{
	if (B.ready)
	{
		hdb_os_log("checkpoint saved: Doom saves too");
		push_halo_event(HDB_EV_CHECKPOINT_SAVED, 0.f, 0, NULL);
	}
}

void hdb_bridge_reverted(void)
{
	if (B.ready)
	{
		hdb_os_log("reverted to the checkpoint: Doom loads its save");
		push_halo_event(HDB_EV_REVERTED, 0.f, 0, NULL);
	}
}

/* ---------- input (hdb_input_sdl.c) */

static int reserved(uint16_t scancode)
{
	int i;

	/* UZDoom's menu or console takes every key (Esc closes it, letters
	type), but fullscreen and the mouse release */
	if (B.doom_menu)
		return scancode == 0x57 || scancode == 0x58;
	for (i = 0; i < B.config.halo_key_count; i++)
		if (B.config.halo_keys[i] == scancode)
			return 1;
	return 0;
}

static int shared_key(uint16_t scancode)
{
	int i;
	for (i = 0; i < B.config.both_key_count; i++)
		if (B.config.both_keys[i] == scancode)
			return 1;
	return 0;
}

int hdb_bridge_key(uint16_t scancode, int down)
{
	if (!B.ready)
		return 0;
	if (!B.doom_menu && shared_key(scancode))
	{
		/* both games (E: Halo's action, Halo Doom's use); Halo sees it too */
		if (down && B.driving && held_find(scancode) < 0 && push_input(HDB_IN_KEY_DOWN, scancode, 0, 0))
			held_add(scancode);
		else if (!down && held_remove(scancode))
			push_input(HDB_IN_KEY_UP, scancode, 0, 0);
		return 0;
	}
	if (reserved(scancode))
		return 0;
	if (!down && held_remove(scancode))
	{
		/* a release Doom saw pressed always goes to Doom */
		push_input(HDB_IN_KEY_UP, scancode, 0, 0);
		return 1;
	}
	if (!down || !B.driving)
		return 0;
	if (!push_input(HDB_IN_KEY_DOWN, scancode, 0, 0))
		return 0;
	held_add(scancode);
	return 1;
}

int hdb_bridge_mouse_move(int32_t dx, int32_t dy)
{
	if (!B.ready || !B.driving)
		return 0;
	push_input(HDB_IN_MOUSE, 0, dx, dy);
	return 1;   /* swallowed even if the ring is full, so Halo does not aim too */
}

int hdb_bridge_mouse_button(uint8_t button, int down)
{
	uint16_t key = (uint16_t)(0x1000u | button);

	if (!B.ready)
		return 0;
	if (!down && held_remove(key))
	{
		push_input(HDB_IN_BTN_UP, button, 0, 0);
		return 1;
	}
	if (!down || !B.driving)
		return 0;
	if (!push_input(HDB_IN_BTN_DOWN, button, 0, 0))
		return 0;
	held_add(key);
	return 1;
}

int hdb_bridge_mouse_wheel(int32_t notches)
{
	if (!B.ready || !B.driving || notches == 0)
		return 0;
	push_input(HDB_IN_WHEEL, 0, 0, notches);
	return 1;
}

#endif /* HALO_HDBRIDGE */
