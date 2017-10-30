takdit: takdit.c
	$(CC) -o takdit takdit.c -Wall -W -pedantic -std=c99

install: takdit
	cp takdit /usr/local/bin/takdit

clean:
	rm takdit