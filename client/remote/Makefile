# Build the remote control client with Open Watcom (wmake).
#
#   wmake
#   wmake clean
#
# Also builds on a Linux host with the Open Watcom V2 snapshot (binl64).

CC = wcc386
RC = wrc
LINKER = wlink

# -za99: C99, -3: 386, -bt=nt: Win32, -bm: multithreaded CRT (poll thread),
# -bg: GUI application, -ox: optimize, -wx: all warnings, -fo=.obj: object
# extension (wcc386 on Linux defaults to .o)
CFLAGS = -za99 -3 -bt=nt -bm -bg -ox -wx -fo=.obj -DCJSON_HIDE_SYMBOLS

TARGET = ClaudeWin9x-Remote.exe
RESOURCE = ClaudeRemote.rc
RESOURCE_RES = ClaudeRemote.res

OBJECTS = remote.obj remote_core.obj handlers.obj http.obj transfer.obj util.obj cJSON.obj

all: $(TARGET)

remote.obj: remote.c remote_core.h resource.h
	$(CC) $(CFLAGS) remote.c

remote_core.obj: remote_core.c remote_core.h ../claude.h ../handlers.h ../http.h ../util.h
	$(CC) $(CFLAGS) remote_core.c

handlers.obj: ../handlers.c ../handlers.h ../claude.h ../util.h ../transfer.h
	$(CC) $(CFLAGS) ../handlers.c

http.obj: ../http.c ../http.h ../claude.h
	$(CC) $(CFLAGS) ../http.c

transfer.obj: ../transfer.c ../transfer.h ../claude.h ../util.h
	$(CC) $(CFLAGS) ../transfer.c

util.obj: ../util.c ../util.h ../claude.h
	$(CC) $(CFLAGS) ../util.c

cJSON.obj: ../third_party/cJSON.c ../third_party/cJSON.h
	$(CC) $(CFLAGS) -fo=cJSON.obj ../third_party/cJSON.c

$(RESOURCE_RES): $(RESOURCE) resource.h ../../build/icons/client.ico
	$(RC) -q -r -bt=nt -fo=$@ $(RESOURCE)

$(TARGET): $(OBJECTS) $(RESOURCE_RES)
	$(LINKER) system nt_win name $@ file {$(OBJECTS)} library ws2_32 option resource=$(RESOURCE_RES)

clean: .SYMBOLIC
	-rm -f *.obj $(RESOURCE_RES) $(TARGET)
