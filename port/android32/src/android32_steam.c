/*
ANDROID32_STEAM.C

The Steam content client's system calls (port/linux/src/steam_client_bridge.h):
the desktop's download of a Steam copy of the game reads Steam's libraries
and runs steamcmd, neither of which an Android box has. The client's own code
is built (the game list needs it), and with these it finds no Steam: nothing
is offered. Host ABI, as posix_steam_bridge.c, which leaves Android out.
*/

#include <dlfcn.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

void *steam_bridge_platform_library_open(const char *path)
{
	(void)path;
	return NULL;
}

void *steam_bridge_platform_library_symbol(void *library, const char *name)
{
	(void)library;
	(void)name;
	return NULL;
}

void steam_bridge_platform_set_app_id(const char *app_id)
{
	(void)app_id;
}

void steam_bridge_platform_sleep(int milliseconds)
{
	if (milliseconds > 0)
		usleep((useconds_t)milliseconds * 1000);
}

int steam_bridge_platform_find_steamcmd(char *path, int size)
{
	(void)path;
	(void)size;
	return 0;
}

int steam_bridge_platform_steamcmd_root(const char *steamcmd, char *root, int size)
{
	(void)steamcmd;
	(void)root;
	(void)size;
	return 0;
}

void *steam_bridge_platform_child_start(const char *const *arguments)
{
	(void)arguments;
	return NULL;
}

int steam_bridge_platform_child_line(void *child, char *line, int size, int milliseconds)
{
	(void)child;
	(void)line;
	(void)size;
	(void)milliseconds;
	return -1;
}

void steam_bridge_platform_child_end(void *child)
{
	(void)child;
}

int steam_bridge_platform_open_sign_in(const char *steamcmd)
{
	(void)steamcmd;
	return 0;
}

int steam_bridge_platform_merge_folder(const char *from, const char *to)
{
	(void)from;
	(void)to;
	return 0;
}

int steam_bridge_platform_remove_folder(const char *path)
{
	(void)path;
	return 0;
}

int steam_bridge_platform_is_folder(const char *path)
{
	struct stat information;

	return path && stat(path, &information) == 0 && S_ISDIR(information.st_mode);
}
