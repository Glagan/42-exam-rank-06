#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>

#define MAX_CLIENTS 10
#define MAX_QUEUE 64
#define BUFFER_SIZE 65535

volatile sig_atomic_t running;

void stop() {
	running = 0;
}

typedef struct s_message {
	size_t sender;
	char* msg;
	size_t length;
} message;

typedef struct s_client {
	size_t id;
	int fd;
	char* rcv_buffer;
	size_t offset;
	size_t total_size;
	message queue[MAX_QUEUE];
	size_t queue_length;
} client;

typedef struct s_state {
	fd_set read;
	fd_set write;
	size_t total;
	client clients[MAX_CLIENTS];
} state;

// Split buf by line and returns the first line or until it's end.
char* extract_message(const char* buffer, size_t length) {
	size_t i = 0;
	char* result = NULL;

	while (i < length) {
		if (buffer[i] == '\n')
			break;
		i++;
	}
	if (!(result = calloc(i + 1, sizeof(char))))
		return NULL;
	if (i > 0)
		memcpy(result, buffer, i);
	result[i] = 0;
	return result;
}

// Join to strings in a newly allocated string.
char* str_join(const char* str1, const char* str2) {
	char* merged = NULL;
	size_t len1 = strlen(str1);
	size_t len2 = strlen(str2);

	if (!(merged = calloc(len1 + len2 + 1, sizeof(char))))
		return NULL;
	memcpy(merged, str1, len1);
	memcpy(merged + len1, str2, len2);

	return merged;
}

int clean_client(client* clt) {
	if (clt->fd)
		close(clt->fd);
	clt->fd = 0;
	if (clt->rcv_buffer)
		free(clt->rcv_buffer);
	size_t j = 0;
	while (j < clt->queue_length) {
		if (clt->queue[j].msg) {
			free(clt->queue[j].msg);
			clt->queue[j].msg = NULL;
		}
		j++;
	}
	clt->rcv_buffer = NULL;
	return 0;
}

int clean_exit(state* server, int sockfd, int return_code) {
	if (server) {
		int i = 0;
		while (i < MAX_CLIENTS)
			clean_client(&server->clients[i++]);
	}
	if (sockfd > 0)
		close(sockfd);
	return return_code;
}

int exit_fatal(state* server, int sockfd, int return_code) {
	write(STDERR_FILENO, "Fatal error\n", 12);
	return clean_exit(server, sockfd, return_code);
}

void add_to_queue(state* server, size_t sender, char* msg, size_t length) {
	size_t i = 0;

	while (i < MAX_CLIENTS) {
		client* clt = server->clients + i;
		if (clt->fd) {
			clt->queue[clt->queue_length].sender = sender;
			clt->queue[clt->queue_length].msg = msg;
			clt->queue[clt->queue_length].length = length;
			clt->queue_length++;
		}
		i++;
	}
}

void pop_client_queue(client* clt) {
	size_t i = 0;
	free(clt->queue[0].msg);
	clt->queue[0].msg = NULL;
	while (i < clt->queue_length) {
		clt->queue[i].sender = clt->queue[i + 1].sender;
		clt->queue[i].msg = clt->queue[i + 1].msg;
		clt->queue[i].length = clt->queue[i + 1].length;
		clt->queue[i + 1].msg = NULL;
		i++;
	}
	clt->queue_length--;
}

int main(int argc, char** argv) {
	if (argc != 2) {
		write(STDERR_FILENO, "Wrong number of arguments !\n", 28);
		return 1;
	}

	// Initialize Socket

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
		exit_fatal(NULL, -1, 1);
	fcntl(sockfd, F_SETFL, O_NONBLOCK);

	int port = atoi(argv[1]);
	struct sockaddr_in self;
	socklen_t len = sizeof(self);
	bzero(&self, len);
	self.sin_family = AF_INET;
	self.sin_addr.s_addr = inet_addr("127.0.0.1");
	self.sin_port = htons(port);

	if (bind(sockfd, (struct sockaddr*)&self, len) != 0)
		exit_fatal(NULL, sockfd, 1);

	if (listen(sockfd, MAX_CLIENTS) != 0)
		exit_fatal(NULL, sockfd, 1);

	printf("Server open on port %d\n", port);

	// Initialize server

	char buffer[BUFFER_SIZE];
	state server;
	int i = 0, j;
	while (i < MAX_CLIENTS) {
		server.clients[i].fd = 0;
		server.clients[i].rcv_buffer = NULL;
		server.clients[i].queue_length = 0;
		j = 0;
		while (j < MAX_QUEUE)
			server.clients[i].queue[j++].msg = NULL;
		i++;
	}

	// Main Loop

	running = 1;
	struct timeval timeout;
	signal(SIGINT, stop);
	while (running) {
		timeout.tv_sec = 4;
		timeout.tv_usec = 0;

		FD_ZERO(&server.read);
		FD_ZERO(&server.write);
		FD_SET(sockfd, &server.read);

		// Add current clients to read and write
		int max = sockfd;
		i = 0;
		while (i < MAX_CLIENTS) {
			client* clt = server.clients + i;
			if (clt->fd) {
				printf("Added client %ld to read (%d)\n", clt->id, clt->fd);
				FD_SET(clt->fd, &server.read);
				if (clt->queue_length > 0) {
					printf("Added client %ld to write (%d)\n", clt->id, clt->fd);
					FD_SET(clt->fd, &server.write);
				}
				if (clt->fd > max)
					max = clt->fd;
			}
			i++;
		}

		// Loop trough existing clients
		printf("waiting for activity up to %d.\n", max + 1);
		int activity = select(max + 1, &server.read, &server.write, NULL, &timeout);
		printf("Select activity: %d\n", activity);
		if (activity < 0)
			exit_fatal(&server, sockfd, 1);
		else if (activity > 0) {
			// New client on read for the server socket
			if (FD_ISSET(sockfd, &server.read)) {
				printf("New client request\n");
				int new_client = accept(sockfd, NULL, NULL);
				if (new_client) {
					fcntl(new_client, F_SETFL, O_NONBLOCK);
					int j = 0;
					while (j < MAX_CLIENTS) {
						if (server.clients[j].fd == 0) {
							printf("Accepted new client %d\n", new_client);
							server.clients[j].id = server.total++;
							server.clients[j].fd = new_client;
							j = MAX_CLIENTS;
						}
						j++;
					}
					if (j == MAX_CLIENTS) {
						printf("Too many clients.\n");
						close(new_client);
					}
				}
			}

			// Handle read and write
			i = 0;
			while (i < MAX_CLIENTS) {
				client* clt = server.clients + i;
				if (!clt->fd) {
					i++;
					continue;
				}
				if (FD_ISSET(clt->fd, &server.read)) {
					printf("Receiving from %ld (%d)\n", clt->id, clt->fd);
					size_t received = recv(clt->fd, buffer, BUFFER_SIZE - 1, MSG_DONTWAIT);
					buffer[received] = 0;
					if (received == 0) {
						printf("Disconnected %ld (%d)\n", clt->id, clt->fd);
						clean_client(&server.clients[i]);
					}
					else {
						size_t offset = 0;
						while (offset < received) {
							char* line = extract_message(buffer, received);
							if (line == NULL)
								return exit_fatal(&server, sockfd, 1);
							size_t length = strlen(line);
							offset += length;
							printf("added to queue `%s` from %ld (%d)\n", line, clt->id, clt->fd);
							if (length > 0)
								add_to_queue(&server, clt->id, line, length);
						}
					}
				}
				else if (FD_ISSET(clt->fd, &server.write)) {
					printf("Sending to client %ld (%d)\n", clt->id, clt->fd);
					send(clt->fd, clt->queue[0].msg, clt->queue[0].length, MSG_DONTWAIT);
					pop_client_queue(clt);
				}
				i++;
			}
		}
		else
			printf("Timed out\n");
	}

	return clean_exit(&server, sockfd, 0);
}
