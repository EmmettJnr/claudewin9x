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
 * Activity notifications for front ends that want to show when work is
 * being done (e.g. an animated icon). Called on the polling thread.
 */
#define HANDLER_ACTIVITY_DONE 0
#define HANDLER_ACTIVITY_WORKING 1 /* an operation arrived, carrying it out */
#define HANDLER_ACTIVITY_SENDING 2 /* sending the result to the server */

typedef void (*HandlerActivityHook)(int activity);

void handlers_set_activity_hook(HandlerActivityHook hook);

/*
 * Run commands in a hidden window instead of the console (for GUI front
 * ends). Off by default.
 */
void handlers_set_hidden_exec(int enabled);

#endif /* HANDLERS_H */
