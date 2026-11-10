CC = cc
CFLAGS = -O2 -std=c11 -Wall -Wextra -Wpedantic
LDLIBS = -lm

.PHONY: all run asset unity-prepare unity-export clean
all: default_dance assets/jonesy-unity.dance

default_dance: defaulty_boi.c
	$(CC) $(CFLAGS) defaulty_boi.c -o $@ $(LDLIBS)

run: default_dance assets/jonesy-unity.dance
	./default_dance

asset: assets/jonesy-unity.dance

assets/jonesy-unity.dance:
	python3 tools/export_unity.py

unity-prepare:
	python3 tools/export_unity.py --prepare-only

unity-export:
	python3 tools/export_unity.py

clean:
	rm -f default_dance
	rm -rf tools/__pycache__
