# FIT3143 Lab 3 Makefile
# Muhammad Mubashir Shah (33878897) & Ayman Zuhair Shashavali (33506728)

CC = mpicc
CFLAGS = -O3 -fopenmp -Wall
LDFLAGS = -lm

TARGETS = task_blocking task_nonblocking

all: $(TARGETS)

task_blocking: task_blocking.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

task_nonblocking: task_nonblocking.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS) *.o base_station_log.txt results_*.csv

.PHONY: all clean
