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

	uint32_t last_doom_heartbeat;
	uint64_t last_doom_change_ms;

	hdb_vec3 motion;    /* this tick's Doom displacement, world units */
	int have_motion;
	int starved;
	float yaw, pitch;
	int crouch;

	uint32_t picture_width, picture_height;
	int have_picture;
	int launch_pending;
	int doom_seen;   /* 0 nothing yet, 1 talking, 2 ready: for the log */
	uint64_t init_ms;

	volatile long input_lock;
	uint16_t held[HDB_MAX_HELD];
	int held_count;
} B;

static hdb_near near_units[HDB_MAX_CANDIDATES];
static int near_count;
static hdb_vec3 near_centre;

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
	for (i = 0; i < B.config.damage_type_count; i++)
	{
		B.damage_hashes[i] = hdb_fnv1a_lower(B.config.damage_type_name[i]);
		B.damage_effects[i] = hdb_game_damage_effect(B.config.damage_effect_path[i]);
		if (B.damage_effects[i] == -1)
			hdb_os_log("damage effect not in this map: %s", B.config.damage_effect_path[i]);
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
	if (have_me)
	{
		near_centre = to_vec3(me.pos);
		hdb_game_for_each_unit(collect_unit, NULL);
		if (near_count > HDB_MAX_PROXIES)
		{
			qsort(near_units, (size_t)near_count, sizeof(near_units[0]), by_distance);
			near_count = HDB_MAX_PROXIES;
		}
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
	s->proxy_count = (uint32_t)near_count;
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
	if (hdb_game_paused()) flags |= HDB_HS_PAUSED;
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

static long effect_for(uint32_t damage_type)
{
	int i;

	for (i = 0; i < B.config.damage_type_count; i++)
		if (B.damage_hashes[i] == damage_type && B.damage_effects[i] != -1)
			return B.damage_effects[i];
	return B.default_damage_effect;
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
			continue;
		if (damage.flags & HDB_DF_NEEDS_LOS)
		{
			/* Doom's void has no walls: Halo's structure between the shot
			and the target's middle decides */
			hdb_game_vec3 from = to_game_vec3(damage.origin), middle = target.pos;
			hdb_ray_hit hit;

			middle.z += target.height * 0.5f;
			if (hdb_game_ray_test(&from, &middle, 0, B.player, &hit) && hit.hit)
				continue;
		}
		{
			hdb_game_vec3 origin = to_game_vec3(damage.origin), direction = to_game_vec3(damage.dir);

			hdb_game_damage_object((long)damage.target_id, damage.amount * B.config.outgoing_damage_scale,
				effect_for(damage.dtype_hash), &origin, &direction);
		}
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

int hdb_bridge_player_damaged(long victim_index, float amount, float const source[3], long damage_effect_index)
{
	hdb_vec3 from;

	if (!B.ready || !B.driving || B.killing || victim_index != B.player || victim_index == -1)
		return 0;
	from.x = source[0];
	from.y = source[1];
	from.z = source[2];
	push_halo_event(HDB_EV_PLAYER_DAMAGED, amount * B.config.incoming_damage_scale, (uint32_t)damage_effect_index, &from);
	return 1;
}

void hdb_bridge_checkpoint_saved(void)
{
	if (B.ready)
		push_halo_event(HDB_EV_CHECKPOINT_SAVED, 0.f, 0, NULL);
}

void hdb_bridge_reverted(void)
{
	if (B.ready)
		push_halo_event(HDB_EV_REVERTED, 0.f, 0, NULL);
}

/* ---------- input (hdb_input_sdl.c) */

static int reserved(uint16_t scancode)
{
	int i;
	for (i = 0; i < B.config.halo_key_count; i++)
		if (B.config.halo_keys[i] == scancode)
			return 1;
	return 0;
}

int hdb_bridge_key(uint16_t scancode, int down)
{
	if (!B.ready || reserved(scancode))
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
