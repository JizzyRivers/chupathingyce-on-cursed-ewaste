/*
STEAMLINK_MEMTRACE.C

Who holds the game's memory, for the box's 263 MB: with HALO_MEM_TRACE=<seconds>
set, every allocation of 4 KB or more that is still held is kept by the
address that asked for it (glibc's malloc hooks, which its 2.19 has), and
that many seconds after the start, and every 60 after, the log gets the
callers holding the most ("memtrace: <KB> in <count> from halo+0x<address>").
The addresses are the executable's, for llvm-symbolizer with the unstripped
build. Without the variable nothing is hooked.
*/

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

/* (glibc's own, under the hooks) */
extern void *__libc_malloc(size_t size);
extern void *__libc_realloc(void *pointer, size_t size);
extern void *__libc_memalign(size_t alignment, size_t size);
extern void __libc_free(void *pointer);
extern void *(*__malloc_hook)(size_t size, const void *caller);
extern void *(*__realloc_hook)(void *pointer, size_t size, const void *caller);
extern void *(*__memalign_hook)(size_t alignment, size_t size, const void *caller);
extern void (*__free_hook)(void *pointer, const void *caller);

enum
{
	TRACE_SLOTS = 1 << 16,
	TRACE_MINIMUM = 4096,
	TRACE_CALLERS = 512,
	TRACE_REPORTED = 40,
};

static struct
{
	void *pointer;
	const void *caller;
	size_t size;
} trace_slots[TRACE_SLOTS];
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;
static Uint64 trace_report_ms;
static unsigned long trace_lost;

static unsigned long trace_slot(const void *pointer)
{
	return (unsigned long)(((uintptr_t)pointer >> 4) * 2654435761u) & (TRACE_SLOTS - 1);
}

static void trace_add(void *pointer, size_t size, const void *caller)
{
	unsigned long slot, tries;

	if (!pointer || size < TRACE_MINIMUM)
		return;
	pthread_mutex_lock(&trace_lock);
	for (slot = trace_slot(pointer), tries = 0; tries < 64; slot = (slot + 1) & (TRACE_SLOTS - 1), tries++)
	{
		if (!trace_slots[slot].pointer)
		{
			trace_slots[slot].pointer = pointer;
			trace_slots[slot].caller = caller;
			trace_slots[slot].size = size;
			break;
		}
	}
	if (tries == 64)
		trace_lost++;
	pthread_mutex_unlock(&trace_lock);
}

static void trace_remove(void *pointer)
{
	unsigned long slot, tries;

	if (!pointer)
		return;
	pthread_mutex_lock(&trace_lock);
	for (slot = trace_slot(pointer), tries = 0; tries < 64; slot = (slot + 1) & (TRACE_SLOTS - 1), tries++)
	{
		if (trace_slots[slot].pointer == pointer)
		{
			/* (kept as a tombstone of size 0: the probe goes on past it) */
			trace_slots[slot].pointer = (void *)1;
			trace_slots[slot].size = 0;
			break;
		}
	}
	pthread_mutex_unlock(&trace_lock);
}

static void trace_report(void)
{
	static struct
	{
		const void *caller;
		size_t bytes;
		unsigned long count;
	} callers[TRACE_CALLERS];
	unsigned long slot, index, used = 0, total = 0, round;

	memset(callers, 0, sizeof(callers));
	pthread_mutex_lock(&trace_lock);
	for (slot = 0; slot < TRACE_SLOTS; slot++)
	{
		if (!trace_slots[slot].size)
			continue;
		total += trace_slots[slot].size;
		for (index = 0; index < used && callers[index].caller != trace_slots[slot].caller; index++)
		{
		}
		if (index == used)
		{
			if (used == TRACE_CALLERS)
				continue;
			callers[used++].caller = trace_slots[slot].caller;
		}
		callers[index].bytes += trace_slots[slot].size;
		callers[index].count++;
	}
	pthread_mutex_unlock(&trace_lock);
	platform_log("memtrace: %lu KB held in allocations of 4 KB or more (%lu not kept)", total / 1024, trace_lost);
	for (round = 0; round < TRACE_REPORTED; round++)
	{
		unsigned long best = 0;

		for (index = 1; index < used; index++)
		{
			if (callers[index].bytes > callers[best].bytes)
				best = index;
		}
		if (!used || callers[best].bytes < 256 * 1024)
			break;
		platform_log("memtrace: %lu KB in %lu from halo+0x%lx", (unsigned long)(callers[best].bytes / 1024),
			callers[best].count, (unsigned long)(uintptr_t)callers[best].caller);
		callers[best].bytes = 0;
	}
}

static void trace_tick(void)
{
	Uint64 now = SDL_GetTicks();

	if (trace_report_ms && now >= trace_report_ms)
	{
		trace_report_ms = now + 60000;
		trace_report();
	}
}

/* glibc's own calls look at the hooks too, so each is made with them off,
one thread at a time (a thread that allocates meanwhile is not kept: few) */
static pthread_mutex_t hook_lock = PTHREAD_MUTEX_INITIALIZER;
static void *trace_malloc(size_t size, const void *caller);
static void *trace_realloc(void *old, size_t size, const void *caller);
static void *trace_memalign(size_t alignment, size_t size, const void *caller);
static void trace_free(void *pointer, const void *caller);

static void hooks_off(void)
{
	pthread_mutex_lock(&hook_lock);
	__malloc_hook = NULL;
	__realloc_hook = NULL;
	__memalign_hook = NULL;
	__free_hook = NULL;
}

static void hooks_on(void)
{
	__malloc_hook = trace_malloc;
	__realloc_hook = trace_realloc;
	__memalign_hook = trace_memalign;
	__free_hook = trace_free;
	pthread_mutex_unlock(&hook_lock);
}

static void *trace_malloc(size_t size, const void *caller)
{
	void *pointer;

	hooks_off();
	pointer = __libc_malloc(size);
	hooks_on();
	trace_add(pointer, size, caller);
	return pointer;
}

static void *trace_realloc(void *old, size_t size, const void *caller)
{
	void *pointer;

	trace_remove(old);
	hooks_off();
	pointer = __libc_realloc(old, size);
	hooks_on();
	trace_add(pointer, size, caller);
	return pointer;
}

static void *trace_memalign(size_t alignment, size_t size, const void *caller)
{
	void *pointer;

	hooks_off();
	pointer = __libc_memalign(alignment, size);
	hooks_on();
	trace_add(pointer, size, caller);
	return pointer;
}

static void trace_free(void *pointer, const void *caller)
{
	(void)caller;
	trace_remove(pointer);
	hooks_off();
	__libc_free(pointer);
	hooks_on();
}

/* each pass of the event pump (steamlink_combos.c) */
void steamlink_memtrace_update(void)
{
	if (trace_report_ms)
		trace_tick();
}

__attribute__((constructor)) static void trace_start(void)
{
	const char *seconds = getenv("HALO_MEM_TRACE");

	if (!seconds || !*seconds)
		return;
	trace_report_ms = (Uint64)strtoul(seconds, NULL, 10) * 1000 + 1;
	__malloc_hook = trace_malloc;
	__realloc_hook = trace_realloc;
	__memalign_hook = trace_memalign;
	__free_hook = trace_free;
}
