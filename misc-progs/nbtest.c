/*
 * nbtest.c -- read and write in non-blocking mode
 * 
 * Most conventional user applications use blocking mode for I/O, this test program can be used
 * to test I/O in non-blocking mode. It just copies from input to output with some delay.
 * 
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>

char buffer[4096];

int main(int argc, char **argv)
{
	int delay = 1, n, m = 0;

	if (argc > 1)
		delay = atoi(argv[1]);
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);	/* stdin */
	fcntl(1, F_SETFL, fcntl(1, F_GETFL) | O_NONBLOCK);	/* stdout */

	while (1) {
		n = read(0, buffer, 4096);
		if (n >= 0)
			m = write(1, buffer, n);
		if ((n < 0 || m < 0) && (errno != EAGAIN))
			break;
		sleep(delay);
	}
	perror(n < 0 ? "stdin" : "stdout");
	exit(1);
}

