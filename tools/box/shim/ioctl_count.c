/* ioctl_count.so (LD_PRELOAD): what the GPU driver asks of the kernel. Every
600 presents' worth of time (5 s) a line a kind of ioctl: its request, the
Vivante command (the first word of the interface the request points at), how
many and how long. Built by build.sh; run with
export LD_PRELOAD=/tmp/trial/ioctl_count.so in launch.env. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define KINDS 96

static struct { unsigned long request; unsigned int command; unsigned long count; unsigned long long ns; } kinds[KINDS];
static int kind_count;
static unsigned long long window_start;

static unsigned long long now_ns(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (unsigned long long)time.tv_sec * 1000000000ull + (unsigned long long)time.tv_nsec;
}

int ioctl(int descriptor, unsigned long request, ...)
{
	static int (*real)(int, unsigned long, void *);
	va_list arguments;
	void *argument;
	unsigned int command = 0;
	unsigned long long start, took;
	int result, index;

	va_start(arguments, request);
	argument = va_arg(arguments, void *);
	va_end(arguments);
	if (!real)
		real = (int (*)(int, unsigned long, void *))dlsym(RTLD_NEXT, "ioctl");
	/* (Vivante's: the argument is { input, size, output, size }, 64-bit
	words or 32; the input's first word is the command) */
	if (request == 30000 && argument)
	{
		const unsigned int *words = argument;
		const unsigned int *input = (const unsigned int *)(unsigned long)words[0];

		if (input)
			command = input[0];
	}
	start = now_ns();
	result = real(descriptor, request, argument);
	took = now_ns() - start;
	for (index = 0; index < kind_count; index++)
	{
		if (kinds[index].request == request && kinds[index].command == command)
			break;
	}
	if (index == kind_count && kind_count < KINDS)
	{
		kinds[index].request = request;
		kinds[index].command = command;
		kinds[index].count = 0;
		kinds[index].ns = 0;
		kind_count++;
	}
	if (index < kind_count)
	{
		kinds[index].count++;
		kinds[index].ns += took;
	}
	if (!window_start)
		window_start = start;
	if (start - window_start > 5000000000ull)
	{
		for (index = 0; index < kind_count; index++)
		{
			if (kinds[index].count)
				fprintf(stderr, "ioctl count: request %lu command %u: %lu in 5 s, %.1f ms\n", kinds[index].request,
					kinds[index].command, kinds[index].count, (double)kinds[index].ns / 1000000.0);
			kinds[index].count = 0;
			kinds[index].ns = 0;
		}
		fprintf(stderr, "ioctl count: --\n");
		window_start = start;
	}
	return result;
}
