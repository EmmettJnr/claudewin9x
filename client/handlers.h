/*
 * handlers.h - Tool approval, file operation, and command handlers
 */

#ifndef HANDLERS_H
#define HANDLERS_H

#include "claude.h"

int poll_approval(void);
int process_approval(void);
int handle_approval(void);

int handle_fileop(void);

int handle_command(void);

/*
 * Run commands in a hidden window instead of the console (for GUI front
 * ends). Off by default.
 */
void handlers_set_hidden_exec(int enabled);

#endif /* HANDLERS_H */
