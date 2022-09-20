#include "heap.h"

#include "debug.h"
#include "tlsf/tlsf.h"

#include <stddef.h>
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct arena_t
{
	pool_t pool;
	struct arena_t* next;
} arena_t;

typedef struct heap_t
{
	tlsf_t tlsf;
	size_t grow_increment;
	arena_t* arena;
} heap_t;

heap_t* heap_create(size_t grow_increment)
{
	debug_print(1, "greetings\n");
	heap_t* heap = VirtualAlloc(NULL, sizeof(heap_t) + tlsf_size(),
		MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!heap)
	{
		debug_print(
			k_print_error,
			"OUT OF MEMORY!\n");
		return NULL;
	}

	heap->grow_increment = grow_increment;
	heap->tlsf = tlsf_create(heap + 1);
	heap->arena = NULL;

	return heap;
}

void* heap_alloc(heap_t* heap, size_t size, size_t alignment)
{
	// increasing size by 128 bytes to hold up to 16 functions worth of call stack
	// tho really 15 because i'm pretty sure the callstack will include heap_alloc here
	// hopefully that should be enough; including more would get *very* expensive i think
	// unfortunately c won't let us do the necessary arithmetic on a void*
	// so we're gonna recast it to a void**, put stack info into the first 64 bytes of that,
	// then return the address after the stack info recast back to a void*
	// something like (void*) (address + 16) i think?
	// actually it might help with these weird issues to store it as a void* here and only
	// cast it to a void** for when arithmetic is necessary?
	// (it didn't lol)
	void* address = tlsf_memalign(heap->tlsf, alignment, size + 128);
	if (!address)
	{
		size_t arena_size =
			__max(heap->grow_increment, (size + 128) * 2) +
			sizeof(arena_t);
		arena_t* arena = VirtualAlloc(NULL,
			arena_size + tlsf_pool_overhead(),
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!arena)
		{
			debug_print(
				k_print_error,
				"OUT OF MEMORY!\n");
			return NULL;
		}

		arena->pool = tlsf_add_pool(heap->tlsf, arena + 1, arena_size);

		arena->next = heap->arena;
		heap->arena = arena;

		address = tlsf_memalign(heap->tlsf, alignment, size + 128);
	}

	// now we have to actually get our callstack
	//int n = debug_backtrace(address, 8);

	return (void*) ((void**) address + 16);
}

void heap_free(heap_t* heap, void* address)
{
	// gotta make some slightly ugly adjustments to the address in order to also free the 64 bytes allocated for the callstack
	tlsf_free(heap->tlsf, (void*) ((void**) address - 16));
}

// so something is causing tlsf to think there's a block of data with some 1.3 gigabytes
// when i run tlsf_walk_pool it tells me i leaked that and then immediately crashes
// this happens even when i replace almost everything else i wrote with the original code from github
// i have no idea why this is happening, i'm losing my mind trying to figure that out, and i'm out of time
// i'm just gonna write the rest of what i was planning as if this wasn't happening, submit that,
// and hope for something resembling partial credit

static void heap_walker(void* ptr, size_t size, int used, void* user)
{
	// adjusting the size again
	debug_print(1, "Memory leak of size %d bytes with callstack:\n", size - 128);

	for (int i = 1; i < 8 && ((void**) ptr + i) != NULL; i++)
	{
		debug_print(1, "[%d] %s\n", i - 1, ((void**) ptr + i));
	}

	heap_free(user, ptr);
}

void heap_destroy(heap_t* heap)
{
	tlsf_destroy(heap->tlsf);

	arena_t* arena = heap->arena;
	while (arena)
	{
		arena_t* next = arena->next;
		
		// use a walker function to look for memory leaks
		tlsf_walk_pool(next, heap_walker, heap);

		VirtualFree(arena, 0, MEM_RELEASE);
		arena = next;
	}

	VirtualFree(heap, 0, MEM_RELEASE);
}
