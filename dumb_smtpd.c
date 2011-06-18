/*
 * Dumb SMTP relaying server
 * Phillip Berndt, 2011
 *
 * What this program does:
 *  Create a minimal SMTP server (which does not even fully comply to RFC 821),
 *  accept all mails matching a particular pattern and forward them to a given
 *  host via UDP without doing any check whether the message has arrived.
 *
 * Intended for use on private routers
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <netdb.h>
#include <pwd.h>
#include <pthread.h>

// Buffer size for reading mails, in Bytes. If the program receives more than
// BUFFER_LEN bytes in a single line, the connection is dropped.
#define BUFFER_LEN 2048

char *mail_match, *target_host, *target_port;

// Send a string to a socket. For convinience.
inline void sendstr(int sock, char *str) {
	send(sock, str, strlen(str), 0);
}

// Handler function for incoming connections
void *client_handler(void *void_sock) {
	int sock = (int)void_sock;
	printf("Thread. Sock is %d\n", sock);

	// Create an UDP socket to the target host
	// Resolve here to allow for dynamic IP changes
	struct sockaddr_in target_addr;
	struct addrinfo *target_addrs;
	struct addrinfo preferred_addr;
	int udp_client = -1;
	memset(&preferred_addr, 0, sizeof(struct addrinfo));
	preferred_addr.ai_family = AF_UNSPEC;
	preferred_addr.ai_socktype = SOCK_DGRAM;
	preferred_addr.ai_protocol = IPPROTO_UDP;
	if(getaddrinfo(target_host, target_port, &preferred_addr, &target_addrs) == 0) {
		udp_client = socket(target_addrs->ai_family, SOCK_DGRAM, IPPROTO_UDP);
	}

	int mode = 0;
	int received = 0;
	char data[BUFFER_LEN + 2];
	data[BUFFER_LEN + 1] = 0;
	int left_over = 0;
	char *current_line, *next_line;

	// SMTP greeting
	sendstr(sock, "220 Dumb SMTP says hello\r\n");

	while(1) {
		// Read data from socket
		if(left_over >= BUFFER_LEN - 1) {
			sendstr(sock, "502 Error: Line too long\r\n");
			break;
		}
		received = recv(sock, data + left_over, BUFFER_LEN - 1 - left_over, 0);
		if(received < 1) break;
		data[left_over + received] = 0;
		current_line = data;

		// Walk through the received lines
		while(next_line = strchr(current_line, '\n')) {
			*next_line = 0;
			// Remove carriage return
			if(next_line - 1 > current_line && *(next_line - 1) == '\r') *(next_line - 1) = 0;

			// Handle current line:
			// We seperately handle normal command mode and the state
			// where DATA is to be received.
			if(mode < 2) {
				// Command mode
				if(current_line[0] == 0) {
					// Ignore empty lines
				}
				else if(strncasecmp(current_line, "DATA", 5) == 0) {
					// DATA command, start of mail
					if(mode == 1) {
						// RCPT has been set
						sendstr(sock, "354 Go ahead, end with <CR><LF>.<CR><LF>\r\n");
						mode = 2;
					}
					else {
						// RCPT has not been set
						sendstr(sock, "503 Must have sender and recipient first.\r\n");
					}
				}
				// Greetings, mail from, reset
				// We don't care. Just reply with a positive answer.
				// Strictly speaking HELO/EHLO require us to reply with the
				// host's name but in practice other servers don't seem to
				// care, they relay their messages anyway.
				else if(strncasecmp(current_line, "EHLO", 4) == 0
					   || strncasecmp(current_line, "HELO", 4) == 0
					   || strncasecmp(current_line, "MAIL FROM", 9) == 0
					   || strncasecmp(current_line, "RSET", 5) == 0) {
					sendstr(sock, "250 localhost\r\n");
				}
				// Receipient
				else if(strncasecmp(current_line, "RCPT TO", 7) == 0) {
					// Do a basic check
					// This is done to avoid being categorized as an open
					// relay
					if(strcasestr(current_line, mail_match) == NULL) {
						sendstr(sock, "550 Rejected\r\n");
						mode = 0;
					}
					else {
						sendstr(sock, "250 Ok\r\n");
						mode = 1;
					}
				}
				else if(strncasecmp(current_line, "QUIT", 5) == 0) {
					sendstr(sock, "221 Bye\r\n");
					goto breakout;
				}
				else {
					sendstr(sock, "502 Command not implemented\r\n");
				}
			}
			else {
				// Data mode

				// Messages end with a line containing a single period
				if(strncmp(current_line, ".", 2) == 0) {
					mode = 0;
					sendstr(sock, "250 Ok\r\n");
				}

				// In this mode, relay the current line to the UDP target
				if(udp_client != -1) {
					int i = strlen(current_line);
					current_line[i] = '\n';
					sendto(udp_client, current_line, i + 1, 0, target_addrs->ai_addr, target_addrs->ai_addrlen);
				}
			}

			current_line = next_line + 1;
		}
		if(*current_line) {
			// If there is data left, because reading ended in the middle of a line,
			// keep it
			strncpy(data, current_line, BUFFER_LEN - 1);
			left_over = strlen(data);
		}
		else {
			left_over = 0;
		}
	}
breakout:

	// Cleanup
	shutdown(sock, SHUT_RDWR);
	close(sock);
	if(udp_client != -1) {
		close(udp_client);
		freeaddrinfo(target_addrs);
	}
}

int main(int argc, char *argv[]) {
	// Parse command line
	if(argc != 5) {
		fprintf(stderr, "Syntax: %s <listen-port> <valid-rcpt-match> <target-host> <target-port>\n", argv[0]);
		exit(1);
	} 
	mail_match = argv[2];
	target_host = argv[3];
	target_port = argv[4];

	// Create a TCP server on port <listen-port>
	int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(server == -1) {
		fprintf(stderr, "Failed to create a socket\n");
		exit(1);
	}
	int on = 1;
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(atoi(argv[1]));
	if(bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "Failed to bind to port %d\n", atoi(argv[1]));
		exit(1);
	}
	listen(server, 2);

	// Setuid/setgid to nobody
	struct passwd *nobody_user = getpwnam("nobody");
	if(nobody_user) {
		chdir(nobody_user->pw_dir);
		if(setgid(nobody_user->pw_gid) == -1) {
			fprintf(stderr, "Warning: setgid() to nobody failed!\n");
		}
		if(setuid(nobody_user->pw_uid) == -1) {
			fprintf(stderr, "Warning: setuid() to nobody failed!\n");
		}
	}
	else {
		fprintf(stderr, "Warning: User nobody not found. NOT setuid()'ing!\n");
	}
	
	// Fork into background
	//daemon(0, 0);

	// Listen forever, let client_handler handle connections
	struct sockaddr_in client_addr;
	int client;
	pthread_t thread_info;
	while(1) {
		client = accept(server, NULL, NULL);
		printf("Sock is %d\n", client);
		pthread_create(&thread_info, NULL, &client_handler, (void*)client);
		// ↑ SIC! We don't send a pointer to client to avoid having to deal with
		// typical threading problems. Also, we don't need to know the thread's
		// ids, so we neglect them.
	}
}
