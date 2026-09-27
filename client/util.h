/*
 * util.c - Utility functions
 */

#ifndef UTIL_H
#define UTIL_H

#include "claude.h"

void path_to_backslashes(char *path);

int build_full_path(const char *relative, char *out, size_t out_size);

void get_windows_version(char *buf, size_t bufsize);

/* Kinds of text passed to the UI output hook */
#define UI_TEXT_CLAUDE 0 /* Claude's responses */
#define UI_TEXT_INFO 1   /* client activity (file ops, commands, config) */
#define UI_TEXT_ERROR 2

/*
 * Output hook for non-console front ends. When unset (the default), text
 * goes to stdout, or stderr for errors.
 */
typedef void (*UiOutputHook)(int kind, const char *text);

void ui_set_output_hook(UiOutputHook hook);

void ui_write(int kind, const char *text);

void ui_printf(const char *fmt, ...);

void print_output(const char *text);

void log_user_input(const char *text);

void log_error(const char *context, const char *message);

void config_load(const char *filename);

#endif /* UTIL_H */
