#include "trace.h"
#include "heap.h"
#include "fs.h"
#include "queue.h"
#include "timer_object.h"

#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// i'm gonna be making a queue for each thread
// i have no idea if this is the right way to do things
// but i don't see any better way to make sure trace_duration_pop gets the thing from the right thread
typedef struct trace_queue_t
{
	queue_t* queue;
	int tid;
} trace_queue_t;

typedef struct trace_t
{
	heap_t* heap;
	fs_t* fs;
	trace_queue_t** queues;
	int num_queues;
	// true if trace_capture_start has been called and trace_capture_stop hasn't
	bool active;
	timer_object_t* timer;
	int capacity;
	// name of the file
	const char* file;
	// stores the individual events as strings in the json format
	queue_t* events;
	int num_events;
	// stores the current work being done by our fs (trying to read and write our file at the same time would mess with things)
	fs_work_t* file_work;
} trace_t;

trace_t* trace_create(heap_t* heap, int event_capacity)
{
	trace_t* trace = heap_alloc(heap, sizeof(trace_t), 8);
	trace->capacity = event_capacity;
	trace->heap = heap;
	trace->num_events = 0;
	// with the way i'm handling different threads i'll only be able to support a set number
	// i hope 16 is good enough
	trace->queues = heap_alloc(heap, 16 * sizeof(trace_queue_t), 8);
	trace->num_queues = 0;
	trace->active = false;
	trace->timer = timer_object_create(heap, NULL);
	// got a weird warning message if i didn't cast event_capacity to a size_t here
	trace->events = queue_create(trace->heap, event_capacity);

	return trace;
}

void trace_destroy(trace_t* trace)
{
	fs_destroy(trace->fs);
	for (int i = 0; i < trace->num_queues; i++)
	{
		queue_push(trace->queues[i]->queue, NULL);
		queue_destroy(trace->queues[i]->queue);
	}
	queue_push(trace->events, NULL);
	queue_destroy(trace->events);
	timer_object_destroy(trace->timer);
}

static trace_queue_t* trace_get_queue(trace_t* trace, int tid)
{
	for (int i = 0; i < trace->num_queues; i++)
	{
		if (trace->queues[i]->tid == tid) return trace->queues[i];
	}
	if (trace->num_queues < 16)
	{
		trace->queues[trace->num_queues] = heap_alloc(trace->heap, sizeof(trace_queue_t), 8);
		trace->queues[trace->num_queues]->tid = tid;
		trace->queues[trace->num_queues]->queue = queue_create(trace->heap, trace->capacity);
		trace->num_queues++;
		return trace->queues[trace->num_queues - 1];
	}
	return NULL;
}

static void trace_f_write(trace_t* trace)
{
	if (trace->file_work != NULL)
	{
		fs_work_wait(trace->file_work);
		fs_work_destroy(trace->file_work);
	}

	trace->file_work = fs_read(trace->fs, trace->file, trace->heap, true, false);
	char* text = fs_work_get_buffer(trace->file_work);
	fs_work_destroy(trace->file_work);

	// i hope this is enough space while also not being too much
	// it's roughly based on 100 * the number of bytes probably needed for one line,
	// rounded up to the nearest power of two
	char* full_text = heap_alloc(trace->heap, strlen(text) + 8196, 8);
	strcpy_s(full_text, strlen(text) + 8196, text);
	for (int i = 0; i < trace->num_events; i++)
	{
		// this is more than will probably be necessary
		char* line = queue_pop(trace->events);
		strcat_s(full_text, strlen(text) + 8196, line);
	}

	trace->file_work = fs_write(trace->fs, trace->file, full_text, strlen(text) + 8196, false);

	fs_work_wait(trace->file_work);
	fs_work_destroy(trace->file_work);
	trace->file_work = NULL;
}

void trace_duration_push(trace_t* trace, const char* name)
{
	if (!trace->active) return;
	int tid = GetCurrentThreadId();
	trace_queue_t* queue = trace_get_queue(trace, tid);
	if (queue == NULL) return;
	uint64_t time = timer_object_get_us(trace->timer);
	queue_push(queue->queue, (void*) name);

	char* buffer = heap_alloc(trace->heap, 52 + strlen(name) , 8);
	sprintf_s(buffer, 52 + strlen(name), "\t\t{\"name\":\"%s\",\"ph\":\"B\",\"pid\":\"0\",\"tid\":\"%d\",\"ts\":\"%llu\"\n", name, tid, time);
	queue_push(trace->events, buffer);
	trace->num_events++;
	if (trace->num_events == trace->capacity) trace_f_write(trace);
}

void trace_duration_pop(trace_t* trace)
{
	if (!trace->active) return;
	int tid = GetCurrentThreadId();
	trace_queue_t* queue = trace_get_queue(trace, tid);
	if (queue == NULL) return;
	uint64_t time = timer_object_get_us(trace->timer);
	char* name = queue_pop(queue->queue);

	char* buffer = heap_alloc(trace->heap, 52 + strlen(name), 8);
	sprintf_s(buffer, 52 + strlen(name), "\t\t{\"name\":\"%s\",\"ph\":\"E\",\"pid\":\"0\",\"tid\":\"%d\",\"ts\":\"%llu\"\n", name, tid, time);
	queue_push(trace->events, buffer);
	if (trace->num_events == trace->capacity) trace_f_write(trace);
}

void trace_capture_start(trace_t* trace, const char* path)
{
	if (trace->active) return;
	trace->fs = fs_create(trace->heap, 1);
	trace->file = path;

	char* top_text = "{\n\t\"displayTimeUnit\": \"ns\", \"traceEvents\": [\n";

	trace->file_work = fs_write(trace->fs, trace->file, top_text, 45, false);

	trace->active = true;
}

void trace_capture_stop(trace_t* trace)
{
	if (!trace->active) return;

	trace_f_write(trace);

	trace->active = false;

	if (trace->file_work != NULL)
	{
		fs_work_wait(trace->file_work);
		fs_work_destroy(trace->file_work);
	}

	trace->file_work = fs_read(trace->fs, trace->file, trace->heap, true, false);
	char* text = fs_work_get_buffer(trace->file_work);
	fs_work_destroy(trace->file_work);
	
	// copy it to a string with a larger buffer and add on the json ending
	char* full_text = heap_alloc(trace->heap, strlen(text) + 5, 8);
	strcpy_s(full_text, strlen(text) + 5, text);
	char* bottom_text = "\t]\n}";
	strcat_s(full_text, strlen(text) + 5, bottom_text);
	
	trace->file_work = fs_write(trace->fs, trace->file, full_text, strlen(text) + 5, false);

	fs_work_wait(trace->file_work);
	fs_work_destroy(trace->file_work);

	heap_free(trace->heap, text);
	heap_free(trace->heap, full_text);
}
