/*
HDB_GAME.C

The HaloDoom bridge's view of the game (port/linux/include/hdb_game.h): the
only bridge file that sees the game's own structures. Built with the game's
sources, so it sees the game exactly as they do.

Campaign only: the bridge stays out of multiplayer games.
*/

#include "../include/hdb_hooks.h"

#ifdef HALO_HDBRIDGE

#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "objects/objects.h"
#include "objects/object_types.h"
#include "objects/damage.h"
#include "objects/damage_effect_definitions.h"
#include "units/units.h"
#include "units/bipeds.h"
#include "units/biped_definitions.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "structures/structure_bsp_definitions.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"
#include "cutscene/cinematics.h"
#include "interface/ui_widget.h"
#include "main/console.h"
#include "math/real_math.h"
#include "../include/hdb_game.h"

extern long halo_screen_width(void);

/* first-person weapon and HUD (render_objects.c, hud.c) */
static int fp_weapon_and_hud_visible = TRUE;

/* ---------- helpers */

static unsigned long hash_lower(char const *text)
{
	unsigned long hash = 2166136261UL;

	for (; text && *text; text++)
	{
		unsigned char c = (unsigned char)*text;

		if (c >= 'A' && c <= 'Z')
			c = (unsigned char)(c - 'A' + 'a');
		hash = (hash ^ c) * 16777619UL;
	}
	return hash & 0xFFFFFFFFUL;
}

static long local_player(void)
{
	return local_player_get_player_index(0);
}

static struct unit_datum *player_unit_datum(void)
{
	long player_index = local_player();
	long unit_index;

	if (player_index == NONE)
		return NULL;
	unit_index = player_get(player_index)->unit_index;
	if (unit_index == NONE)
		return NULL;
	return (struct unit_datum *)object_try_and_get_and_verify_type(unit_index, _object_mask_unit);
}

/* ---------- game state */

int hdb_game_in_progress(void)
{
	if (!game_in_progress() || main_menu_is_active() || game_engine_running())
		return FALSE;
	return player_unit_datum() != NULL;
}

int hdb_game_paused(void)
{
	return main_menu_is_active() || console_is_active() || (game_in_progress() && game_time_get_paused());
}

int hdb_game_cinematic(void)
{
	return cinematic_in_progress() || (players_globals && players_globals->input_disabled);
}

char const *hdb_game_map_name(void)
{
	return global_scenario_index != NONE ? tag_get_name(global_scenario_index) : NULL;
}

long hdb_game_bsp_index(void)
{
	return global_structure_bsp_index_get();
}

int hdb_game_bsp_bounds(hdb_game_vec3 *minimum, hdb_game_vec3 *maximum)
{
	struct structure_bsp *bsp = global_structure_bsp_get();

	if (!bsp)
		return FALSE;
	minimum->x = bsp->world_bounds.x0;
	minimum->y = bsp->world_bounds.y0;
	minimum->z = bsp->world_bounds.z0;
	maximum->x = bsp->world_bounds.x1;
	maximum->y = bsp->world_bounds.y1;
	maximum->z = bsp->world_bounds.z1;
	return TRUE;
}

float hdb_game_fov_horizontal(void)
{
	/* as main.c builds the camera: a vertical field of view from the unit's
	(or zoomed weapon's) tag value, then as wide as the picture
	(halo_screen_width x 480) */
	real field_of_view = player_control_get_field_of_view(0);
	real vertical_tangent = 0.75f * tangent(field_of_view * 0.5f) * 0.85f;
	real aspect = (real)halo_screen_width() / 480.f;

	return 2.f * arctangent(vertical_tangent * aspect, 1.f);
}

/* ---------- local player 0 */

long hdb_game_player_unit(void)
{
	long player_index = local_player();

	return player_index != NONE ? player_get(player_index)->unit_index : NONE;
}

int hdb_game_player_in_vehicle(void)
{
	struct unit_datum *unit = player_unit_datum();

	return unit && unit->object.parent_object_index != NONE;
}

int hdb_game_player_dead(void)
{
	struct unit_datum *unit = player_unit_datum();

	return !unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit);
}

int hdb_game_player_grounded(void)
{
	long unit_index = hdb_game_player_unit();
	struct biped_datum *biped;

	if (unit_index == NONE)
		return FALSE;
	biped = (struct biped_datum *)object_try_and_get_and_verify_type(unit_index, _object_mask_biped);
	return biped && !TEST_FLAG(biped->biped.flags, _biped_airborne_bit);
}

int hdb_game_player_in_water(void)
{
	return FALSE;
}

int hdb_game_get_unit(long handle, hdb_unit_info *info)
{
	struct unit_datum *unit = (struct unit_datum *)object_try_and_get_and_verify_type(handle, _object_mask_unit);
	struct _object_datum const *object;
	real shields;

	csmemset(info, 0, sizeof(*info));
	if (!unit)
		return FALSE;
	object = &unit->object;
	info->handle = handle;
	info->pos.x = object->position.x;
	info->pos.y = object->position.y;
	info->pos.z = object->position.z;
	info->vel.x = object->translational_velocity.i;
	info->vel.y = object->translational_velocity.j;
	info->vel.z = object->translational_velocity.k;
	info->yaw = arctangent(object->forward.j, object->forward.i);
	info->team = object->owner_team_index;
	info->kind_hash = hash_lower(tag_get_name(unit->definition_index));
	shields = object->maximum_shield_vitality > 0.f ? 1.f : 0.f;
	info->health_frac = (object->body_vitality + (shields > 0.f ? object->shield_vitality : 0.f)) / (1.f + shields);
	info->alive = !TEST_FLAG(object->damage_flags, _object_dead_bit);
	info->is_vehicle = object_try_and_get_and_verify_type(handle, _object_mask_vehicle) != NULL;
	if (info->is_vehicle)
	{
		/* its bounding sphere, standing on the ground under it */
		info->radius = object->bounding_sphere_radius;
		info->height = 2.f * object->bounding_sphere_radius;
		info->pos.z = object->bounding_sphere_center.z - object->bounding_sphere_radius;
	}
	else
	{
		/* the biped's collision height, standing to crouched, from its
		feet (its origin) up. Not biped_get_physics_pill: that gives the
		pill's straight part for players only, and no height at all for
		AI, which made every enemy's proxy too flat to hit. */
		struct biped_datum *biped = (struct biped_datum *)object_try_and_get_and_verify_type(handle, _object_mask_biped);

		if (biped)
		{
			struct biped_definition *definition = biped_definition_get(biped->definition_index);
			real standing = definition->biped.collision_height_standing;
			real crouching = definition->biped.collision_height_crouching;

			info->radius = definition->biped.collision_radius;
			info->height = standing + (crouching - standing) * biped->biped.crouch;
			if (info->height < 2.f * info->radius)
				info->height = 2.f * info->radius;
			if (TEST_FLAG(definition->biped.flags, _biped_pill_centered_at_origin_bit))
				info->pos.z -= info->height * 0.5f;
		}
		else
		{
			/* any other unit: its bounding sphere, as a vehicle */
			info->radius = object->bounding_sphere_radius;
			info->height = 2.f * object->bounding_sphere_radius;
			info->pos.z = object->bounding_sphere_center.z - object->bounding_sphere_radius;
		}
	}
	return TRUE;
}

void hdb_game_for_each_unit(hdb_unit_fn fn, void *context)
{
	struct object_iterator iterator;

	object_iterator_new(&iterator, _object_mask_unit, 0);
	while (object_iterator_next(&iterator))
		fn(iterator.index, context);
}

/* ---------- presentation */

void hdb_game_set_fp_weapon_and_hud_visible(int visible)
{
	fp_weapon_and_hud_visible = visible;
}

int hdb_game_fp_weapon_and_hud_visible(void)
{
	return fp_weapon_and_hud_visible;
}

/* ---------- world */

int hdb_game_ray_test(hdb_game_vec3 const *from, hdb_game_vec3 const *to, int include_objects,
	long ignore_object, hdb_ray_hit *hit)
{
	/* the line-of-sight test: the structure (front faces, solid, not the
	invisible or two-sided surfaces), and with objects the ones that block
	sight (vehicles, scenery, machines) */
	unsigned long flags = include_objects ? _collision_test_for_line_of_sight_flags :
		(_collision_test_for_line_of_sight_flags & ~(FLAG(_collision_test_objects_bit) | _collision_test_objects_all_types_flags));
	struct collision_result result;
	real_point3d point;
	real_vector3d vector;

	csmemset(hit, 0, sizeof(*hit));
	hit->entity = NONE;
	point.x = from->x;
	point.y = from->y;
	point.z = from->z;
	vector.i = to->x - from->x;
	vector.j = to->y - from->y;
	vector.k = to->z - from->z;
	if (collision_test_vector(flags, &point, &vector, ignore_object, &result))
	{
		hit->hit = TRUE;
		hit->entity = result.object_index;
		hit->point.x = result.point.x;
		hit->point.y = result.point.y;
		hit->point.z = result.point.z;
		hit->normal.x = result.plane.n.i;
		hit->normal.y = result.plane.n.j;
		hit->normal.z = result.plane.n.k;
	}
	return TRUE;
}

long hdb_game_damage_effect(char const *tag_path)
{
	/* the campaign's own bullets, for when hdbridge.ini names none or a
	tag this map lacks */
	static char const *const fallbacks[] =
	{
		"weapons\\assault rifle\\bullet",
		"weapons\\pistol\\bullet",
		"weapons\\plasma rifle\\bolt",
		"weapons\\plasma pistol\\bolt",
	};
	long index = NONE;
	short i;

	if (tag_path && tag_path[0])
		index = tag_loaded(DAMAGE_EFFECT_DEFINITION_TAG, tag_path);
	for (i = 0; index == NONE && i < (short)NUMBEROF(fallbacks); i++)
		index = tag_loaded(DAMAGE_EFFECT_DEFINITION_TAG, fallbacks[i]);
	return index;
}

char const *hdb_game_tag_name(long tag_index)
{
	char const *name = tag_index != NONE ? tag_get_name(tag_index) : NULL;

	return name ? name : "?";
}

void hdb_game_damage_object(long target, float amount, long damage_effect,
	hdb_game_vec3 const *origin, hdb_game_vec3 const *direction)
{
	long player_index = local_player();
	struct object_datum *object = (struct object_datum *)object_try_and_get_and_verify_type(target, _object_mask_unit);
	struct damage_effect_definition *definition;
	struct damage_data damage;
	real typical;

	if (damage_effect == NONE)
		damage_effect = hdb_game_damage_effect(NULL);
	if (!object || damage_effect == NONE || player_index == NONE || amount <= 0.f)
		return;

	/* the effect's own damage, scaled to Doom's amount; dealt by the player,
	so the AI reacts, talks and credits the kill as for Halo's weapons */
	definition = damage_effect_definition_get(damage_effect);
	typical = 0.5f * (definition->damage.damage_lower_bound + definition->damage.damage_upper_bound);
	if (typical <= 0.f)
		typical = definition->damage.damage_minimum > 0.f ? definition->damage.damage_minimum : 1.f;

	damage_data_new(&damage, damage_effect);
	damage.owner_player_index = player_index;
	damage.owner_object_index = player_get(player_index)->unit_index;
	damage.owner_team_index = (short)player_get(player_index)->team_index;
	damage.location = object->object.location;
	damage.origin.x = origin->x;
	damage.origin.y = origin->y;
	damage.origin.z = origin->z;
	damage.epicenter = damage.origin;
	damage.direction.i = direction->x;
	damage.direction.j = direction->y;
	damage.direction.k = direction->z;
	damage.scale = 1.f;
	damage.multiplier = amount / typical;
	object_cause_damage(&damage, target, NONE, NONE, NONE, NULL);
}

void hdb_game_kill_player(void)
{
	long unit_index = hdb_game_player_unit();

	if (unit_index != NONE)
		unit_kill(unit_index);
}

#endif /* HALO_HDBRIDGE */
