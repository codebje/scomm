all: scomm

scomm: scomm.o trs20.o
	$(CC) -o $@ -lreadline $^

sercat: sercat.o trs20.o
	$(CC) -o $@ $^

ymsend: ymsend.o trs20.o
	$(CC) -o $@ $^

clean:
	rm -f scomm ymsend sercat *.o
