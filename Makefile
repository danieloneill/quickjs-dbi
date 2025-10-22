QJSPATH=../../quickjs-2025-09-13
#QJSPATH=../../quickjs-2023-12-09
#QJSPATH=../../quickjs-2021-03-27

CC=gcc
CFLAGS=-I$(QJSPATH) $(shell pkg-config --cflags dbi) -fPIC -g
LDFLAGS=$(shell pkg-config --libs dbi)

dbi.so: quickjs-dbi.o
	$(CC) $(CFLAGS) -shared -o dbi.so quickjs-dbi.o $(LDFLAGS)

quickjs-dbi.o: quickjs-dbi.c
	$(CC) $(CFLAGS) -c -o quickjs-dbi.o quickjs-dbi.c

clean:
	rm dbi.so quickjs-dbi.o

all: dbi.so
