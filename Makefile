takdit: takdit.c
	$(CC) takdit.c -o takdit -Wall -Wextra -pedantic -std=c99

clean:
	rm takdit