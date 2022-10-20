#include "fs.h"

#include "event.h"
#include "heap.h"
#include "queue.h"
#include "thread.h"
#include "lz4/lz4.h"

#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// seems easiest to keep the compression stuff in here rather than creating a new struct for it
typedef struct fs_t
{
	heap_t* heap;
	queue_t* file_queue;
	thread_t* file_thread;
	queue_t* compression_queue;
	thread_t* compression_thread;
} fs_t;

typedef enum fs_work_op_t
{
	k_fs_work_op_read,
	k_fs_work_op_write,
} fs_work_op_t;

typedef struct fs_work_t
{
	heap_t* heap;
	// including a pointer to the fs itself to make moving to another queue easier
	fs_t* fs;
	fs_work_op_t op;
	char path[1024];
	bool null_terminate;
	bool use_compression;
	void* buffer;
	size_t size;
	event_t* done;
	int result;
} fs_work_t;

static int compression_thread_func(void* user);
static int file_thread_func(void* user);

fs_t* fs_create(heap_t* heap, int queue_capacity)
{
	fs_t* fs = heap_alloc(heap, sizeof(fs_t), 8);
	fs->heap = heap;
	fs->file_queue = queue_create(heap, queue_capacity);
	fs->file_thread = thread_create(file_thread_func, fs);
	fs->compression_queue = queue_create(heap, queue_capacity);
	fs->compression_thread = thread_create(compression_thread_func, fs);
	return fs;
}

void fs_destroy(fs_t* fs)
{
	queue_push(fs->file_queue, NULL);
	queue_push(fs->compression_queue, NULL);
	thread_destroy(fs->file_thread);
	queue_destroy(fs->file_queue);
	thread_destroy(fs->compression_thread);
	queue_destroy(fs->compression_queue);
	heap_free(fs->heap, fs);
}

fs_work_t* fs_read(fs_t* fs, const char* path, heap_t* heap, bool null_terminate, bool use_compression)
{
	fs_work_t* work = heap_alloc(fs->heap, sizeof(fs_work_t), 8);
	work->done = event_create();
	work->heap = heap;
	work->fs = fs;
	work->op = k_fs_work_op_read;
	strcpy_s(work->path, sizeof(work->path), path);
	work->buffer = NULL;
	work->size = 0;
	work->result = 0;
	work->null_terminate = null_terminate;
	work->use_compression = use_compression;
	queue_push(fs->file_queue, work);
	return work;
}

fs_work_t* fs_write(fs_t* fs, const char* path, const void* buffer, size_t size, bool use_compression)
{
	fs_work_t* work = heap_alloc(fs->heap, sizeof(fs_work_t), 8);
	work->done = event_create();
	work->heap = fs->heap;
	work->fs = fs;
	work->op = k_fs_work_op_write;
	strcpy_s(work->path, sizeof(work->path), path);
	work->buffer = (void*)buffer;
	work->size = size;
	work->result = 0;
	work->null_terminate = false;
	work->use_compression = use_compression;

	if (use_compression)
	{
		queue_push(fs->compression_queue, work);
	}
	else
	{
		queue_push(fs->file_queue, work);
	}

	return work;
}

bool fs_work_is_done(fs_work_t* work)
{
	return work ? event_is_raised(work->done) : true;
}

void fs_work_wait(fs_work_t* work)
{
	if (work)
	{
		event_wait(work->done);
	}
}

int fs_work_get_result(fs_work_t* work)
{
	fs_work_wait(work);
	return work ? work->result : -1;
}

void* fs_work_get_buffer(fs_work_t* work)
{
	fs_work_wait(work);
	return work ? work->buffer : NULL;
}

size_t fs_work_get_size(fs_work_t* work)
{
	fs_work_wait(work);
	return work ? work->size : 0;
}

void fs_work_destroy(fs_work_t* work)
{
	if (work)
	{
		event_wait(work->done);
		event_destroy(work->done);
		heap_free(work->heap, work);
	}
}

static void file_decompress(fs_work_t* work)
{
	char* compressed_buffer = work->buffer;
	// ok let's start by grabbing those file sizes we so sloppily stored
	int compressed_size = 0;
	int decompressed_size = 0;
	for (int i = 0; i < 10; i++)
	{
		compressed_size *= 10;
		decompressed_size *= 10;
		compressed_size += (compressed_buffer[i] - '0');
		decompressed_size += (compressed_buffer[i + 11] - '0');
	}

	// we might want an extra character for the null termination
	char* decompressed_buffer = heap_alloc(work->heap, work->null_terminate ? decompressed_size + 1 : decompressed_size, 8);

	LZ4_decompress_safe(compressed_buffer + 22, decompressed_buffer, compressed_size, decompressed_size);

	if (work->null_terminate) decompressed_buffer[decompressed_size] = '\0';

	work->buffer = decompressed_buffer;
	heap_free(work->heap, compressed_buffer);
	work->size = decompressed_size;
	event_signal(work->done);
}

static void file_read(fs_work_t* work)
{
	wchar_t wide_path[1024];
	if (MultiByteToWideChar(CP_UTF8, 0, work->path, -1, wide_path, sizeof(wide_path)) <= 0)
	{
		work->result = -1;
		event_signal(work->done);
		return;
	}

	HANDLE handle = CreateFile(wide_path, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
	{
		work->result = GetLastError();
		event_signal(work->done);
		return;
	}

	if (!GetFileSizeEx(handle, (PLARGE_INTEGER)&work->size))
	{
		work->result = GetLastError();
		CloseHandle(handle);
		event_signal(work->done);
		return;
	}

	work->buffer = heap_alloc(work->heap, work->null_terminate ? work->size + 1 : work->size, 8);

	DWORD bytes_read = 0;
	if (!ReadFile(handle, work->buffer, (DWORD)work->size, &bytes_read, NULL))
	{
		work->result = GetLastError();
		CloseHandle(handle);
		event_signal(work->done);
		return;
	}

	work->size = bytes_read;
	if (work->null_terminate)
	{
		((char*)work->buffer)[bytes_read] = 0;
	}

	CloseHandle(handle);

	if (work->use_compression)
	{
		queue_push(work->fs->compression_queue, work);
	}
	else
	{
		event_signal(work->done);
	}
}

static void file_compress(fs_work_t* work)
{
	// so i need to work out how best to store the compressed and uncompressed sizes
	// it looks like the maximum input size lz4 can work with is a little over 2 gb/2 billion bytes
	// it might be *best* to store the sizes in binary directly, which could be done with like 8 bytes
	// at the start of the file. however, i'm worried that could cause something somewhere to break
	// in some way involving a string termination character. so instead i'm just storing the sizes in
	// plaintext, which uses like 22 bytes the way i'm implementing it.
	// given the sizes of the files this might be working with i'm not too concerned about this
	
	// let's start by getting our max size and allocating a new buffer
	int max_size = LZ4_compressBound((int) work->size);

	// i tried to do something weird with pointers sort of like this on the last homework
	// and it was a disaster. but i'm trying it again. maybe it'll go better this time
	// what i'm gonna try doing is passing in the pointer offset by 22 bytes into LZ4
	// so i can use the first 22 bytes to store size data
	char* buffer = heap_alloc(work->heap, max_size + 22, 8);

	// compress the data, store the actual compressed size
	int compressed_size = LZ4_compress_default(work->buffer, buffer + 22, (int) work->size, max_size);
	// make a copy of the decompressed size so we can mess with it as we work through storing it
	int decompressed_size = (int) work->size;

	work->size = compressed_size + 22;

	// now store the compressed and uncompressed sizes in plaintext
	// there's probably a better way to do this
	// i'm just getting every digit and writing to the corresponding character in buffer
	// library functions??? what are those???
	for (int i = 9; i >= 0; i--)
	{
		buffer[i] = '0' + (compressed_size % 10);
		buffer[i + 11] = '0' + (decompressed_size % 10);
		compressed_size /= 10;
		decompressed_size /= 10;
	}

	// these newlines don't really serve a purpose other than making me feel better
	buffer[10] = '\n';
	buffer[21] = '\n';

	// i'm allowed to just swap out the buffer here right?
	// like this won't mess with anything on the user's end right?
	// they'll still have their copy of the pointer to free it right?
	work->buffer = buffer;

	queue_push(work->fs->file_queue, work);
}

int get_hash(void* address, int bucket_count)
{
	return (intptr_t)address % bucket_count;
}

static void file_write(fs_work_t* work)
{
	wchar_t wide_path[1024];
	if (MultiByteToWideChar(CP_UTF8, 0, work->path, -1, wide_path, sizeof(wide_path)) <= 0)
	{
		work->result = -1;
		event_signal(work->done);
		return;
	}

	HANDLE handle = CreateFile(wide_path, GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
	{
		work->result = GetLastError();
		event_signal(work->done);
		return;
	}

	DWORD bytes_written = 0;
	if (!WriteFile(handle, work->buffer, (DWORD)work->size, &bytes_written, NULL))
	{
		work->result = GetLastError();
		CloseHandle(handle);
		event_signal(work->done);
		return;
	}

	work->size = bytes_written;

	CloseHandle(handle);

	event_signal(work->done);
}

// this is basically the same as file_thread_func
static int compression_thread_func(void* user)
{
	fs_t* fs = user;
	while (true)
	{
		fs_work_t* work = queue_pop(fs->compression_queue);
		if (work == NULL)
		{
			break;
		}

		switch (work->op)
		{
		case k_fs_work_op_read:
			file_decompress(work);
			break;
		case k_fs_work_op_write:
			file_compress(work);
			break;
		}
	}
	return 0;
}

static int file_thread_func(void* user)
{
	fs_t* fs = user;
	while (true)
	{
		fs_work_t* work = queue_pop(fs->file_queue);
		if (work == NULL)
		{
			break;
		}
		
		switch (work->op)
		{
		case k_fs_work_op_read:
			file_read(work);
			break;
		case k_fs_work_op_write:
			file_write(work);
			break;
		}
	}
	return 0;
}
