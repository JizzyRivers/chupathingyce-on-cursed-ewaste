/*
ANDROID32_SETUP.C

The start-up screens a console port supplies (HALO_CONSOLE_SETUP_SCREENS,
halo_console_features.h), which turns the desktop launcher off: Android has
no install screens yet. The game data is put in the app's external files
folder by hand (adb, or a file manager); without it src/android32_main.c
shows a message and quits. The updater asks nothing: this port has no
release of its own to update from yet.
*/

#include "platform.h"

void console_setup_start(void)
{
}

void console_setup_after_data(const char *data_root)
{
	(void)data_root;
}

BOOL platform_offer_game_data(char *destination, unsigned long size)
{
	(void)destination;
	(void)size;
	return FALSE;
}

void *console_setup_screen_open(void)
{
	return NULL;
}

void console_setup_screen_progress(void *screen, float fraction, const char *title, const char *line)
{
	(void)screen;
	(void)fraction;
	(void)title;
	(void)line;
}

void console_setup_screen_close(void *screen)
{
	(void)screen;
}

void console_setup_notice(const char *first, const char *second)
{
	platform_log("%s %s", first ? first : "", second ? second : "");
}

int console_setup_update_question(const char *latest, const char *current)
{
	(void)latest;
	(void)current;
	return 0;
}
