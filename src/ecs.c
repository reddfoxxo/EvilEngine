#include "ecs.h"

#include "debug.h"
#include "heap.h"
#include "fs.h"

#include <string.h>
#include <stdio.h>

enum
{
	k_max_component_types = 64,
	k_max_entities = 512,
};

typedef enum entity_state_t
{
	k_entity_unused,
	k_entity_pending_add,
	k_entity_active,
	k_entity_pending_remove,
} entity_state_t;

typedef struct ecs_t
{
	heap_t* heap;
	int global_sequence; // 4 bytes

	int sequences[k_max_entities]; // 4 * k_max_entities bytes
	entity_state_t entity_states[k_max_entities];
	uint64_t component_masks[k_max_entities];

	void* components[k_max_component_types];
	size_t component_type_sizes[k_max_component_types];
	char component_type_names[k_max_component_types][32];
	bool save_component[k_max_component_types];
} ecs_t;

// there was a smarter way to do this that was discussed in class.
// naturally, i don't remember it, and google has been unhelpful
// so we get to do it the stupid way
char* convert_to_char_array_stupid(void* num)
{
	// why is torturing pointers my go-to solution for seemingly every problem
	char* result = (char*)num;
	return result;
}

void ecs_save_game(heap_t* heap, ecs_t* ecs, fs_t* fs)
{
	// get the total amount of data that needs to be stored
	size_t size = 4 + ((4 + sizeof(entity_state_t) + 8) * k_max_entities);
	for (int i = 0; i < k_max_component_types; i++)
	{
		if (ecs->save_component[i]) size += (k_max_entities * ecs->component_type_sizes[i]);
	}

	char* data = heap_alloc(heap, size, 8);

	// keeps track of where we are in the file
	size_t ctr = 0;

	// global_sequence
	char* gs = convert_to_char_array_stupid(&ecs->global_sequence);
	for (int i = 0; i < 4; i++) data[i] = gs[i];
	ctr += 4;

	// sequences[]
	for (int i = 0; i < k_max_entities; i++)
	{
		char* sequence = convert_to_char_array_stupid(&ecs->sequences[i]);
		for (int j = 0; j < 4; j++) data[j + ctr] = sequence[j];
		ctr += 4;
	}

	// entity_states[]
	for (int i = 0; i < k_max_entities; i++)
	{
		char* entity_state = convert_to_char_array_stupid(&ecs->entity_states[i]);
		for (int j = 0; j < sizeof(entity_state_t); j++) data[j + ctr] = entity_state[j];
		ctr += sizeof(entity_state_t);
	}

	// component_masks[]
	for (int i = 0; i < k_max_entities; i++)
	{
		char* cm = convert_to_char_array_stupid(&ecs->component_masks[i]);
		for (int j = 0; j < 8; j++) data[j + ctr] = cm[j];
		ctr += 8;
	}

	// now for the tricky part: components
	for (int i = 0; i < k_max_component_types; i++)
	{
		if (ecs->save_component[i])
		{
			size_t component_size = ecs->component_type_sizes[i];
			char* components = (char*)ecs->components[i];
			for (int j = 0; j < k_max_entities; j++)
			{
				for (int k = 0; k < component_size; k++) data[k + ctr] = components[(j * component_size) + k];
				ctr += component_size;
			}
		}
	}

	fs_work_t* work = fs_write(fs, "savegame", data, size, false);
	fs_work_wait(work);
	fs_work_destroy(work);
	heap_free(heap, data);
}

void ecs_load_game(heap_t* heap, ecs_t* ecs, fs_t* fs)
{
	// get the total amount of data that needs to be stored
	size_t size = 4 + ((4 + sizeof(entity_state_t) + 8) * k_max_entities);
	for (int i = 0; i < k_max_component_types; i++)
	{
		size += (k_max_entities * ecs->component_type_sizes[i]);
	}

	fs_work_t* work = fs_read(fs, "savegame", heap, false, false);
	fs_work_wait(work);
	if (fs_work_get_result(work) != 0) return;
	char* data = (char*) fs_work_get_buffer(work);

	// keeps track of where we are in the file
	size_t ctr = 0;

	// global_sequence
	char* gs = data + ctr;
	ecs->global_sequence = *((int*)gs);
	ctr += 4;

	// sequences[]
	for (int i = 0; i < k_max_entities; i++)
	{
		char* sequence = data + ctr;
		ecs->sequences[i] = *((int*)sequence);
		ctr += 4;
	}

	// entity_states[]
	for (int i = 0; i < k_max_entities; i++)
	{
		char* entity_state = data + ctr;
		ecs->entity_states[i] = *((entity_state_t*)entity_state);
		ctr += sizeof(entity_state_t);
	}

	// component_masks[]
	for (int i = 0; i < k_max_entities; i++)
	{
		char* cm = data + ctr;
		ecs->component_masks[i] = *((uint64_t*)cm);
		ctr += 8;
	}

	// now for the tricky part: components
	for (int i = 0; i < k_max_component_types; i++)
	{
		/*
		if (component_size != 0)
		{
			char* components = data + ctr;
			for (int j = 0; j < k_max_entities; j++)
			{
				char** ec = (char**)ecs->components;
				if (ecs->entity_states[i] == k_entity_active && i == 0)
				{
					//printf("%c\n", ec[i][j * component_size + 4]);
					//printf("%c\n", components[j * component_size + 4]);
				}
				ec[i][j * component_size] = components[j * component_size];
				ctr += component_size;
			}
		}
		*/
		if (ecs->save_component[i])
		{
			char* components = data + ctr;
			ecs->components[i] = data + ctr;
			size_t component_size = ecs->component_type_sizes[i];
			ctr += component_size * k_max_entities;
		}
	}
	 
	fs_work_destroy(work);
	heap_free(heap, data);
}

ecs_t* ecs_create(heap_t* heap)
{
	ecs_t* ecs = heap_alloc(heap, sizeof(ecs_t), 8);
	memset(ecs, 0, sizeof(*ecs));
	ecs->heap = heap;
	ecs->global_sequence = 1;
	return ecs;
}

void ecs_destroy(ecs_t* ecs)
{
	for (int i = 0; i < _countof(ecs->components); ++i)
	{
		if (ecs->components[i])
		{
			heap_free(ecs->heap, ecs->components[i]);
		}
	}
	heap_free(ecs->heap, ecs);
}

void ecs_update(ecs_t* ecs)
{
	for (int i = 0; i < _countof(ecs->entity_states); ++i)
	{
		if (ecs->entity_states[i] == k_entity_pending_add)
		{
			ecs->entity_states[i] = k_entity_active;
		}
		else if (ecs->entity_states[i] == k_entity_pending_remove)
		{
			ecs->entity_states[i] = k_entity_unused;
		}
	}
}

int ecs_register_component_type(ecs_t* ecs, const char* name, size_t size_per_component, size_t alignment, bool save)
{
	for (int i = 0; i < _countof(ecs->components); ++i)
	{
		if (ecs->components[i] == NULL)
		{
			size_t aligned_size = (size_per_component + (alignment - 1)) & ~(alignment - 1);
			strcpy_s(ecs->component_type_names[i], sizeof(ecs->component_type_names[i]), name);
			ecs->component_type_sizes[i] = aligned_size;
			ecs->components[i] = heap_alloc(ecs->heap, aligned_size * k_max_entities, alignment);
			memset(ecs->components[i], 0, aligned_size * k_max_entities);
			ecs->save_component[i] = save;
			return i;
		}
	}
	debug_print(k_print_warning, "Out of component types.");
	return -1;
}

size_t ecs_get_component_type_size(ecs_t* ecs, int component_type)
{
	return ecs->component_type_sizes[component_type];
}

ecs_entity_ref_t ecs_entity_add(ecs_t* ecs, uint64_t component_mask)
{
	for (int i = 0; i < _countof(ecs->entity_states); ++i)
	{
		if (ecs->entity_states[i] == k_entity_unused)
		{
			ecs->entity_states[i] = k_entity_pending_add;
			ecs->sequences[i] = ecs->global_sequence++;
			ecs->component_masks[i] = component_mask;
			return (ecs_entity_ref_t) { .entity = i, .sequence = ecs->sequences[i] };
		}
	}
	debug_print(k_print_warning, "Out of entities.");
	return (ecs_entity_ref_t) { .entity = -1, .sequence = -1 };
}

void ecs_entity_remove(ecs_t* ecs, ecs_entity_ref_t ref, bool allow_pending_add)
{
	if (ecs_is_entity_ref_valid(ecs, ref, allow_pending_add))
	{
		ecs->entity_states[ref.entity] = k_entity_pending_remove;
	}
	else
	{
		debug_print(k_print_warning, "Attempting to remove inactive entity.");
	}
}

bool ecs_is_entity_ref_valid(ecs_t* ecs, ecs_entity_ref_t ref, bool allow_pending_add)
{
	return ref.entity >= 0 &&
		ecs->sequences[ref.entity] == ref.sequence &&
		ecs->entity_states[ref.entity] >= (allow_pending_add ? k_entity_pending_add : k_entity_active);
}

void* ecs_entity_get_component(ecs_t* ecs, ecs_entity_ref_t ref, int component_type, bool allow_pending_add)
{
	if (ecs_is_entity_ref_valid(ecs, ref, allow_pending_add) && ecs->components[component_type])
	{
		char* components = ecs->components[component_type];
		return &components[ecs->component_type_sizes[component_type] * ref.entity];
	}
	return NULL;
}

ecs_query_t ecs_query_create(ecs_t* ecs, uint64_t mask)
{
	ecs_query_t query = { .component_mask = mask, .entity = -1 };
	ecs_query_next(ecs, &query);
	return query;
}

bool ecs_query_is_valid(ecs_t* ecs, ecs_query_t* query)
{
	return query->entity >= 0;
}

void ecs_query_next(ecs_t* ecs, ecs_query_t* query)
{
	for (int i = query->entity + 1; i < _countof(ecs->component_masks); ++i)
	{
		if ((ecs->component_masks[i] & query->component_mask) == query->component_mask && ecs->entity_states[i] >= k_entity_active)
		{
			query->entity = i;
			return;
		}
	}
	query->entity = -1;
}

void* ecs_query_get_component(ecs_t* ecs, ecs_query_t* query, int component_type)
{
	char* components = ecs->components[component_type];
	return &components[ecs->component_type_sizes[component_type] * query->entity];
}

ecs_entity_ref_t ecs_query_get_entity(ecs_t* ecs, ecs_query_t* query)
{
	return (ecs_entity_ref_t) { .entity = query->entity, .sequence = ecs->sequences[query->entity] };
}
