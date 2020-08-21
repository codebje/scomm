all: scomm

scomm: scomm.o trs20.o
	$(CC) -o $@ -lreadline $^

clean:
	rm -f scomm *.o
