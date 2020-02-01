
# William Emmanuel
# CS-6210 Spring 2020
# wemmanuel3@gatech.edu
#
# Prelab Makefile used as a template for this project

CC = gcc
CFLAGS = -g -Wall
CPPFLAGS =
LDFLAGS = -lpthread -lvirt

all: vcpu_scheduler

vcpu_scheduler: vcpu_scheduler.c
	$(CC) -o $@ $< $(CFLAGS) $(LDFLAGS)

clean:
	$(RM) -f *.o vcpu_scheduler

run: all
	./vcpu_scheduler 1

debug: all
	gdb vcpu_scheduler
