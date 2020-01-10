all:
	gcc client.c -c
	gcc db.c -c
	gcc comm.c -c
	gcc db.o comm.o server.c -o server -lpthread

