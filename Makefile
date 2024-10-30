
CC=gcc
CFLAGS=-Wall -g -lpthread

all:shell

shell: shell.o SimpleScheduler.o
	$(CC) $(CFLAGS) -o shell shell.o SimpleScheduler.o 

shell.o:shell.c
	$(CC) $(CFLAGS) -c shell.c
SimpleScheduler.o:SimpleScheduler.c
	$(CC) $(CFLAGS) -c SimpleScheduler.c


clean:
	rm -f *.o shell