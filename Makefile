CFLAGS = -g

all: m2mtest

clean:
	rm -f *.o m2mtest

.c.o:
	$(CC) $(CFLAGS) -c $<

m2mtest: main.o h264parser.o
	$(CC) $(CFLAGS) -o $@ $^
