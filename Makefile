takdit: takdit.c
	$(CC) -o takdit takdit.c -Wall -W -pedantic -std=c99

clean:
	rm takdit