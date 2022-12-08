#include "frogger_game.h"

#include "debug.h"
#include "ecs.h"
#include "fs.h"
#include "gpu.h"
#include "heap.h"
//#include "net.h"
#include "render.h"
#include "timer_object.h"
#include "transform.h"
#include "wm.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include <string.h>

typedef struct transform_component_t
{
	transform_t transform;
} transform_component_t;

typedef struct camera_component_t
{
	mat4f_t projection;
	mat4f_t view;
} camera_component_t;

typedef struct model_component_t
{
	gpu_mesh_info_t* mesh_info;
	gpu_shader_info_t* shader_info;
} model_component_t;

typedef struct player_component_t
{
	int index;
} player_component_t;

typedef struct block_component_t
{
	float scale;
} block_component_t;

typedef struct name_component_t
{
	char name[32];
} name_component_t;

typedef struct frogger_game_t
{
	heap_t* heap;
	fs_t* fs;
	wm_window_t* window;
	render_t* render;
	//net_t* net;

	timer_object_t* timer;

	ecs_t* ecs;
	int transform_type;
	int camera_type;
	int model_type;
	int player_type;
	int block_type;
	int name_type;
	ecs_entity_ref_t player_ent;
	ecs_entity_ref_t camera_ent;
	ecs_entity_ref_t block_ents[12];

	gpu_mesh_info_t cube_mesh;
	gpu_shader_info_t cube_shader;
	gpu_mesh_info_t block_mesh;
	fs_work_t* vertex_shader_work;
	fs_work_t* fragment_shader_work;
} frogger_game_t;

static void load_resources(frogger_game_t* game);
static void unload_resources(frogger_game_t* game);
static void spawn_player(frogger_game_t* game, int index);
static void spawn_blocks(frogger_game_t* game);
static void spawn_camera(frogger_game_t* game);
static void update_players(frogger_game_t* game);
static void update_blocks(frogger_game_t* game);
static void draw_models(frogger_game_t* game);

frogger_game_t* frogger_game_create(heap_t* heap, fs_t* fs, wm_window_t* window, render_t* render, int argc, const char** argv)
{
	frogger_game_t* game = heap_alloc(heap, sizeof(frogger_game_t), 8);
	game->heap = heap;
	game->fs = fs;
	game->window = window;
	game->render = render;

	game->timer = timer_object_create(heap, NULL);
	
	game->ecs = ecs_create(heap);
	game->transform_type = ecs_register_component_type(game->ecs, "transform", sizeof(transform_component_t), _Alignof(transform_component_t), true);
	game->camera_type = ecs_register_component_type(game->ecs, "camera", sizeof(camera_component_t), _Alignof(camera_component_t), false);
	game->model_type = ecs_register_component_type(game->ecs, "model", sizeof(model_component_t), _Alignof(model_component_t), false);
	game->player_type = ecs_register_component_type(game->ecs, "player", sizeof(player_component_t), _Alignof(player_component_t), true);
	game->block_type = ecs_register_component_type(game->ecs, "block", sizeof(block_component_t), _Alignof(block_component_t), true);
	game->name_type = ecs_register_component_type(game->ecs, "name", sizeof(name_component_t), _Alignof(name_component_t), false);

	/*
	game->net = net_create(heap, game->ecs);
	if (argc >= 2)
	{
		net_address_t server;
		if (net_string_to_address(argv[1], &server))
		{
			net_connect(game->net, &server);
		}
		else
		{
			debug_print(k_print_error, "Unable to resolve server address: %s\n", argv[1]);
		}
	}
	*/

	load_resources(game);
	spawn_player(game, 0);
	spawn_blocks(game);
	spawn_camera(game);

	return game;
}

void frogger_game_destroy(frogger_game_t* game)
{
	//net_destroy(game->net);
	ecs_destroy(game->ecs);
	timer_object_destroy(game->timer);
	unload_resources(game);
	heap_free(game->heap, game);
}

void frogger_game_update(frogger_game_t* game)
{
	timer_object_update(game->timer);
	ecs_update(game->ecs);
	update_blocks(game);
	update_players(game);
	draw_models(game);
	render_push_done(game->render);
}

static void load_resources(frogger_game_t* game)
{
	game->vertex_shader_work = fs_read(game->fs, "shaders/triangle.vert.spv", game->heap, false, false);
	game->fragment_shader_work = fs_read(game->fs, "shaders/triangle.frag.spv", game->heap, false, false);
	game->cube_shader = (gpu_shader_info_t)
	{
		.vertex_shader_data = fs_work_get_buffer(game->vertex_shader_work),
		.vertex_shader_size = fs_work_get_size(game->vertex_shader_work),
		.fragment_shader_data = fs_work_get_buffer(game->fragment_shader_work),
		.fragment_shader_size = fs_work_get_size(game->fragment_shader_work),
		.uniform_buffer_count = 1,
	};

	static vec3f_t cube_verts[] =
	{
		{ -0.2f, -0.2f,  0.2f }, { 0.0f, 0.2f,  0.2f },
		{  0.2f, -0.2f,  0.2f }, { 0.2f, 0.0f,  0.2f },
		{  0.2f,  0.2f,  0.2f }, { 0.2f, 0.2f,  0.0f },
		{ -0.2f,  0.2f,  0.2f }, { 0.2f, 0.0f,  0.0f },
		{ -0.2f, -0.2f, -0.2f }, { 0.0f, 0.2f,  0.0f },
		{  0.2f, -0.2f, -0.2f }, { 0.0f, 0.0f,  0.2f },
		{  0.2f,  0.2f, -0.2f }, { 0.2f, 0.2f,  0.2f },
		{ -0.2f,  0.2f, -0.2f }, { 0.0f, 0.0f,  0.0f },
	};
	static uint16_t cube_indices[] =
	{
		0, 1, 2,
		2, 3, 0,
		1, 5, 6,
		6, 2, 1,
		7, 6, 5,
		5, 4, 7,
		4, 0, 3,
		3, 7, 4,
		4, 5, 1,
		1, 0, 4,
		3, 2, 6,
		6, 7, 3
	};
	game->cube_mesh = (gpu_mesh_info_t)
	{
		.layout = k_gpu_mesh_layout_tri_p444_c444_i2,
		.vertex_data = cube_verts,
		.vertex_data_size = sizeof(cube_verts),
		.index_data = cube_indices,
		.index_data_size = sizeof(cube_indices),
	};

	static vec3f_t block_verts[] =
	{
		{ -0.2f, -0.2f,  0.2f }, { 0.0f, 0.2f,  0.2f },
		{  0.2f, -0.2f,  0.2f }, { 0.2f, 0.0f,  0.2f },
		{  0.2f,  0.2f,  0.2f }, { 0.2f, 0.2f,  0.0f },
		{ -0.2f,  0.2f,  0.2f }, { 0.2f, 0.0f,  0.0f },
		{ -0.2f, -0.2f, -0.2f }, { 0.0f, 0.2f,  0.0f },
		{  0.2f, -0.2f, -0.2f }, { 0.0f, 0.0f,  0.2f },
		{  0.2f,  0.2f, -0.2f }, { 0.2f, 0.2f,  0.2f },
		{ -0.2f,  0.2f, -0.2f }, { 0.0f, 0.0f,  0.0f },
	};
	static uint16_t block_indices[] =
	{
		0, 1, 2,
		2, 3, 0,
		1, 5, 6,
		6, 2, 1,
		7, 6, 5,
		5, 4, 7,
		4, 0, 3,
		3, 7, 4,
		4, 5, 1,
		1, 0, 4,
		3, 2, 6,
		6, 7, 3
	};
	game->block_mesh = (gpu_mesh_info_t)
	{
		.layout = k_gpu_mesh_layout_tri_p444_c444_i2,
		.vertex_data = block_verts,
		.vertex_data_size = sizeof(block_verts),
		.index_data = block_indices,
		.index_data_size = sizeof(block_indices),
	};
}

static void unload_resources(frogger_game_t* game)
{
	heap_free(game->heap, fs_work_get_buffer(game->vertex_shader_work));
	heap_free(game->heap, fs_work_get_buffer(game->fragment_shader_work));
	fs_work_destroy(game->fragment_shader_work);
	fs_work_destroy(game->vertex_shader_work);
}

/*
static void player_net_configure(ecs_t* ecs, ecs_entity_ref_t entity, int type, void* user)
{
	simple_game_t* game = user;

	model_component_t* model_comp = ecs_entity_get_component(ecs, entity, game->model_type, true);
	model_comp->mesh_info = &game->cube_mesh;
	model_comp->shader_info = &game->cube_shader;
}
*/

static void spawn_blocks(frogger_game_t* game)
{
	for (int i = 0; i < 12; i++)
	{
		uint64_t k_block_ent_mask =
			(1ULL << game->transform_type) |
			(1ULL << game->model_type) |
			(1ULL << game->block_type) |
			(1ULL << game->name_type);
		game->block_ents[i] = ecs_entity_add(game->ecs, k_block_ent_mask);

		transform_component_t* transform_comp = ecs_entity_get_component(game->ecs, game->block_ents[i], game->transform_type, true);
		transform_identity(&transform_comp->transform);

		// set up block positions
		// first row
		if (i <= 3)
		{
			transform_comp->transform.translation = vec3f_add(transform_comp->transform.translation, vec3f_scale(vec3f_up(), 2));
		}
		// second row
		else if (i <= 7)
		{
			transform_comp->transform.translation = vec3f_add(transform_comp->transform.translation, vec3f_scale(vec3f_up(), 0));
		}
		// third row
		else
		{
			transform_comp->transform.translation = vec3f_add(transform_comp->transform.translation, vec3f_scale(vec3f_up(), -2));
		}
		// currently i think the 6.0f gets us a total length for the block's path of 24
		// so blocks reset after going 12 units to a side from the center
		// may have to adjust this in testing to make sure blocks are off the screen before they disappear
		transform_comp->transform.translation = vec3f_add(transform_comp->transform.translation, vec3f_scale(vec3f_right(), (1.5f - (i % 4)) * 6.0f));

		name_component_t* name_comp = ecs_entity_get_component(game->ecs, game->block_ents[i], game->name_type, true);
		strcpy_s(name_comp->name, sizeof(name_comp->name), "block");

		block_component_t* block_comp = ecs_entity_get_component(game->ecs, game->block_ents[i], game->block_type, true);
		//player_comp->index = index;
		// randomize the block's width
		block_comp->scale = ((((float) rand() / RAND_MAX) * 2) + 1) * 4; // float between 4 and 12, may adjust
		transform_comp->transform.scale.y = block_comp->scale;

		model_component_t* model_comp = ecs_entity_get_component(game->ecs, game->block_ents[i], game->model_type, true);
		model_comp->mesh_info = &game->block_mesh;
		model_comp->shader_info = &game->cube_shader;
	}
}

static void spawn_player(frogger_game_t* game, int index)
{
	uint64_t k_player_ent_mask =
		(1ULL << game->transform_type) |
		(1ULL << game->model_type) |
		(1ULL << game->player_type) |
		(1ULL << game->name_type);
	game->player_ent = ecs_entity_add(game->ecs, k_player_ent_mask);

	transform_component_t* transform_comp = ecs_entity_get_component(game->ecs, game->player_ent, game->transform_type, true);
	transform_identity(&transform_comp->transform);
	transform_comp->transform.translation = vec3f_add(transform_comp->transform.translation, vec3f_scale(vec3f_up(), 4));

	name_component_t* name_comp = ecs_entity_get_component(game->ecs, game->player_ent, game->name_type, true);
	strcpy_s(name_comp->name, sizeof(name_comp->name), "player");

	player_component_t* player_comp = ecs_entity_get_component(game->ecs, game->player_ent, game->player_type, true);
	player_comp->index = index;

	model_component_t* model_comp = ecs_entity_get_component(game->ecs, game->player_ent, game->model_type, true);
	model_comp->mesh_info = &game->cube_mesh;
	model_comp->shader_info = &game->cube_shader;

	/*
	uint64_t k_player_ent_net_mask =
		(1ULL << game->transform_type) |
		(1ULL << game->model_type) |
		(1ULL << game->name_type);
	uint64_t k_player_ent_rep_mask =
		(1ULL << game->transform_type);
	*/
	//net_state_register_entity_type(game->net, 0, k_player_ent_net_mask, k_player_ent_rep_mask, player_net_configure, game);

	//net_state_register_entity_instance(game->net, 0, game->player_ent);
}

static void spawn_camera(frogger_game_t* game)
{
	uint64_t k_camera_ent_mask =
		(1ULL << game->camera_type) |
		(1ULL << game->name_type);
	game->camera_ent = ecs_entity_add(game->ecs, k_camera_ent_mask);

	name_component_t* name_comp = ecs_entity_get_component(game->ecs, game->camera_ent, game->name_type, true);
	strcpy_s(name_comp->name, sizeof(name_comp->name), "camera");

	camera_component_t* camera_comp = ecs_entity_get_component(game->ecs, game->camera_ent, game->camera_type, true);
	mat4f_make_orthographic(&camera_comp->projection, (float)M_PI / 2.0f, 16.0f / 9.0f, 0.1f, 100.0f);

	vec3f_t eye_pos = vec3f_scale(vec3f_forward(), -5.0f);
	vec3f_t forward = vec3f_forward();
	vec3f_t up = vec3f_up();
	mat4f_make_lookat(&camera_comp->view, &eye_pos, &forward, &up);
}

static void update_blocks(frogger_game_t* game)
{
	// ok this is completely off topic but i need some positivity in my life:
	// now that i'm finally actually working on this i do feel like i'm starting to see glimpses of "the old me"
	// maybe the last 4 semesters and all the disasters in my personal life haven't broken me quite as permanently as i thought
	// i'm taking a leave of absence next semester and maybe that'll actually help get me back on my feet
	float dt = (float)timer_object_get_delta_ms(game->timer) * 0.001f;
	float speed = dt * 2;

	uint64_t k_query_mask = (1ULL << game->transform_type) | (1ULL << game->block_type);

	for (ecs_query_t query = ecs_query_create(game->ecs, k_query_mask);
		ecs_query_is_valid(game->ecs, &query);
		ecs_query_next(game->ecs, &query))
	{
		transform_component_t* transform_comp = ecs_query_get_component(game->ecs, &query, game->transform_type);
		block_component_t* block_comp = ecs_query_get_component(game->ecs, &query, game->block_type);

		if (block_comp != NULL)
		{
			// middle row goes left to right
			// outer rows go right to left
			// i'm using this < 1, > -1 thing to determine if it's in the middle row because while the z values *should* be locked at
			// -2, 0, and 2, *i don't trust them*
			if (transform_comp->transform.translation.y > 12.0f && transform_comp->transform.translation.z < 1.0f && transform_comp->transform.translation.z > -1.0f)
			{
				transform_comp->transform.translation.y = -12.0f;
				block_comp->scale = ((((float)rand() / RAND_MAX) * 2) + 1) * 4;
			}
			if (transform_comp->transform.translation.y < -12.0f && (transform_comp->transform.translation.z > 1.0f || transform_comp->transform.translation.z < -1.0f))
			{
				transform_comp->transform.translation.y = 12.0f;
				block_comp->scale = ((((float)rand() / RAND_MAX) * 2) + 1) * 4;
			}

			// aggressively make sure our scaling is right every frame
			// i think this'll only matter when a block resets and when we load a save file
			transform_comp->transform.scale.y = block_comp->scale;
			
			transform_t move;
			transform_identity(&move);
			// bottom row
			if (transform_comp->transform.translation.z > 1.0f)
			{
				move.translation = vec3f_add(move.translation, vec3f_scale(vec3f_right(), -speed));
			}
			// top row
			// i think some of the gaps for these are blatantly impossible
			// but i actually kind of like that since the player then has to judge whether they can get away with rushing
			else if (transform_comp->transform.translation.z < -1.0f)
			{
				move.translation = vec3f_add(move.translation, vec3f_scale(vec3f_right(), -speed * 2.0f));
			}
			// middle row
			else
			{
				move.translation = vec3f_add(move.translation, vec3f_scale(vec3f_right(), speed * 1.5f));
			}
			transform_multiply(&transform_comp->transform, &move);
		}
	}
}

static void update_players(frogger_game_t* game)
{
	float dt = (float)timer_object_get_delta_ms(game->timer) * 0.001f;
	float speed = dt * 2;

	uint32_t key_mask = wm_get_key_mask(game->window);
	uint32_t mouse_mask = wm_get_mouse_mask(game->window);

	uint64_t k_query_mask = (1ULL << game->transform_type) | (1ULL << game->player_type);

	for (ecs_query_t query = ecs_query_create(game->ecs, k_query_mask);
		ecs_query_is_valid(game->ecs, &query);
		ecs_query_next(game->ecs, &query))
	{
		transform_component_t* transform_comp = ecs_query_get_component(game->ecs, &query, game->transform_type);
		player_component_t* player_comp = ecs_query_get_component(game->ecs, &query, game->player_type);

		if (player_comp != NULL)
		{
			if (transform_comp->transform.translation.z < -4.0f)
			{
				transform_identity(&transform_comp->transform);
				transform_comp->transform.translation = vec3f_add(transform_comp->transform.translation, vec3f_scale(vec3f_up(), 4));
			}

			// detect collisions
			float player_y = transform_comp->transform.translation.y;
			float player_z = transform_comp->transform.translation.z;
			uint64_t k_block_query_mask = (1ULL << game->transform_type) | (1ULL << game->block_type);
			for (ecs_query_t block_query = ecs_query_create(game->ecs, k_block_query_mask);
				ecs_query_is_valid(game->ecs, &block_query);
				ecs_query_next(game->ecs, &block_query))
			{
				transform_component_t* block_transform_comp = ecs_query_get_component(game->ecs, &block_query, game->transform_type);
				block_component_t* block_comp = ecs_query_get_component(game->ecs, &block_query, game->block_type);
				if (block_comp != NULL)
				{
					float block_y = block_transform_comp->transform.translation.y;
					float block_z = block_transform_comp->transform.translation.z;
					float block_width = block_comp->scale;

					// i think this is right
					// it looks like a lot but any other way i could have done it probably would have required more code
					if (((block_y - (0.2 * block_width)) <= (player_y + 0.2) && (block_y + (0.2 * block_width)) >= (player_y - 0.2)) &&
						((block_z - 0.2) <= (player_z + 0.2) && (block_z + 0.2) >= (player_z - 0.2)))
					{
						transform_identity(&transform_comp->transform);
						transform_comp->transform.translation = vec3f_add(transform_comp->transform.translation, vec3f_scale(vec3f_up(), 4));
					}
				}
			}

			transform_t move;
			transform_identity(&move);
			if (key_mask & k_key_up)
			{
				move.translation = vec3f_add(move.translation, vec3f_scale(vec3f_up(), -speed));
			}
			if (key_mask & k_key_down)
			{
				move.translation = vec3f_add(move.translation, vec3f_scale(vec3f_up(), speed));
			}
			if (key_mask & k_key_left)
			{
				move.translation = vec3f_add(move.translation, vec3f_scale(vec3f_right(), -speed));
			}
			if (key_mask & k_key_right)
			{
				move.translation = vec3f_add(move.translation, vec3f_scale(vec3f_right(), speed));
			}
			if (mouse_mask & k_mouse_button_left)
			{
				ecs_save_game(game->heap, game->ecs, game->fs);
			}
			if (mouse_mask & k_mouse_button_right)
			{
				ecs_load_game(game->heap, game->ecs, game->fs);
			}
			transform_multiply(&transform_comp->transform, &move);
		}
	}
}

static void draw_models(frogger_game_t* game)
{
	uint64_t k_camera_query_mask = (1ULL << game->camera_type);
	for (ecs_query_t camera_query = ecs_query_create(game->ecs, k_camera_query_mask);
		ecs_query_is_valid(game->ecs, &camera_query);
		ecs_query_next(game->ecs, &camera_query))
	{
		camera_component_t* camera_comp = ecs_query_get_component(game->ecs, &camera_query, game->camera_type);

		uint64_t k_model_query_mask = (1ULL << game->transform_type) | (1ULL << game->model_type);
		for (ecs_query_t query = ecs_query_create(game->ecs, k_model_query_mask);
			ecs_query_is_valid(game->ecs, &query);
			ecs_query_next(game->ecs, &query))
		{
			transform_component_t* transform_comp = ecs_query_get_component(game->ecs, &query, game->transform_type);
			model_component_t* model_comp = ecs_query_get_component(game->ecs, &query, game->model_type);
			ecs_entity_ref_t entity_ref = ecs_query_get_entity(game->ecs, &query);

			struct
			{
				mat4f_t projection;
				mat4f_t model;
				mat4f_t view;
			} uniform_data;
			uniform_data.projection = camera_comp->projection;
			uniform_data.view = camera_comp->view;
			transform_to_matrix(&transform_comp->transform, &uniform_data.model);
			gpu_uniform_buffer_info_t uniform_info = { .data = &uniform_data, sizeof(uniform_data) };

			render_push_model(game->render, &entity_ref, model_comp->mesh_info, model_comp->shader_info, &uniform_info);
		}
	}
}
