# Build Tools
CC 	:= gcc
CFLAGS := -g -I/usr/include/gstreamer-0.10 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/include/libxml2 -O2 -Wall -fPIC -DPIC
LD := gcc
LDFLAGS := -O2 -Wall -shared -lasound -lgstfft-0.10

SND_PCM_OBJECTS = ufoleds.o
SND_PCM_BIN = libasound_module_pcm_ufoleds.so

.PHONY: all clean

all: Makefile $(SND_PCM_BIN)

$(SND_PCM_BIN): $(SND_PCM_OBJECTS)
	$(LD) $(LDFLAGS) $(SND_PCM_OBJECTS) -o $(SND_PCM_BIN)

%.o: %.c
	$(CC) -c $(CFLAGS) $<

clean:
	rm -vf *.o *.so
