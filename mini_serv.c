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

#define BUFFER_SIZE 65535

volatile sig_atomic_t running;

void stop() {
	running = 0;
}

typedef struct s_message {
	char* content;
	size_t sender;
	size_t length;
	size_t offset;
	struct s_message* next;
} message;

typedef struct s_client {
	size_t id;
	int fd;
	char* buffer;
	message* queue;
	struct s_client* next;
} client;

typedef struct s_state {
	size_t total;
	client* clients;
} state;

// Split buf by line and returns the first line or until it's end.
int extract_message(const char* buffer, char** stk) {
	int i = 0;
	char* cpy = NULL;

	while (buffer[i]) {
		if (buffer[i] == '\n') {
			if (!(cpy = calloc(i + 1, sizeof(char))))
				return -1;
			memcpy(cpy, buffer, i + 1);
			cpy[i + 1] = 0;
			*stk = cpy;
			return 1;
		}
		i++;
	}
	return 0;
}

// Join to strings in a newly allocated string.
char* str_join(char* str1, char* str2) {
	char* merged = NULL;
	size_t len1 = strlen(str1);
	size_t len2 = strlen(str2);

	if (!(merged = calloc(len1 + len2 + 1, sizeof(char))))
		return NULL;
	memcpy(merged, str1, len1);
	memcpy(merged + len1, str2, len2);
	free(str1);

	return merged;
}

client* clean_client(client* clt) {
	client* next_client = clt->next;
	message* curr_msg = clt->queue;
	while (curr_msg) {
		message* next_msg = curr_msg->next;
		free(curr_msg->content);
		curr_msg->content = NULL;
		curr_msg->next = NULL;
		free(curr_msg);
		curr_msg = next_msg;
	}
	clt->queue = NULL;
	if (clt->buffer)
		free(clt->buffer);
	clt->buffer = NULL;
	clt->next = NULL;
	free(clt);
	return next_client;
}

int broadcast(state* server, size_t sender, char* content, size_t length) {
	client* curr = server->clients;
	while (curr) {
		if (curr->id != sender) {
			message* msg = NULL;
			if (!(msg = (message*)malloc(sizeof(message))))
				return 1;
			if (!(msg->content = (char*)malloc(length + 1))) {
				free(msg);
				return 1;
			}
			strcpy(msg->content, content);
			msg->content[length] = 0;
			msg->length = length;
			msg->next = NULL;
			msg->offset = 0;
			msg->sender = sender;
			if (!curr->queue)
				curr->queue = msg;
			else {
				message* curr_msg = curr->queue;
				while (curr_msg->next)
					curr_msg = curr_msg->next;
				curr_msg->next = msg;
			}
		}
		curr = curr->next;
	}
	return 0;
}

int clean_exit(state* server, int sockfd, int return_code) {
	if (server) {
		client* clt = server->clients;
		while (clt)
			clt = clean_client(clt);
		server->clients = NULL;
	}
	close(sockfd);
	return return_code;
}

int exit_fatal(state* server, int sockfd) {
	write(STDERR_FILENO, "Fatal error\n", 12);
	return clean_exit(server, sockfd, 1);
}

int main(int argc, char** argv) {
	if (argc != 2) {
		write(STDERR_FILENO, "Wrong number of arguments !\n", 28);
		return 1;
	}

	// Initialize Socket

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
		exit_fatal(NULL, -1);
	fcntl(sockfd, F_SETFL, O_NONBLOCK);

	int port = atoi(argv[1]);
	struct sockaddr_in self;
	socklen_t len = sizeof(self);
	bzero(&self, len);
	self.sin_family = AF_INET;
	self.sin_addr.s_addr = inet_addr("127.0.0.1");
	self.sin_port = htons(port);

	if (bind(sockfd, (struct sockaddr*)&self, len) != 0)
		exit_fatal(NULL, sockfd);

	if (listen(sockfd, 10) != 0)
		exit_fatal(NULL, sockfd);

	printf("Server open on port %d\n", port);

	// Initialize server

	char buffer[BUFFER_SIZE];
	char recv_buffer[BUFFER_SIZE];
	state server;
	server.clients = NULL;
	server.total = 0;
	fd_set reads;
	fd_set writes;

	// Main Loop

	running = 1;
	signal(SIGINT, stop);
	while (running) {
		FD_ZERO(&reads);
		FD_ZERO(&writes);
		FD_SET(sockfd, &reads);

		// Add current clients to read and write
		int max = sockfd;
		client* clt = server.clients;
		while (clt) {
			FD_SET(clt->fd, &reads);
			if (clt->queue)
				FD_SET(clt->fd, &writes);
			if (clt->fd > max)
				max = clt->fd;
			clt = clt->next;
		}

		// Loop trough existing clients
		int activity = select(max + 1, &reads, &writes, NULL, NULL);
		if (activity < 0)
			exit_fatal(&server, sockfd);
		else if (activity > 0) {
			// New client on read for the server socket
			if (FD_ISSET(sockfd, &reads)) {
				int new_client = accept(sockfd, NULL, NULL);
				if (new_client) {
					fcntl(new_client, F_SETFL, O_NONBLOCK);
					client* clt = NULL;
					if (!(clt = (client*)malloc(sizeof(client))))
						return exit_fatal(&server, sockfd);
					clt->id = server.total++;
					clt->fd = new_client;
					clt->buffer = NULL;
					clt->queue = NULL;
					clt->next = NULL;
					if (!server.clients)
						server.clients = clt;
					else {
						size_t length = sprintf(buffer, "server: client %ld just arrived\n", clt->id);
						if (broadcast(&server, clt->id, buffer, length))
							return exit_fatal(&server, sockfd);
						client* curr = server.clients;
						while (curr->next)
							curr = curr->next;
						curr->next = clt;
					}
				}
			}

			// Handle read and write
			client* previous = NULL;
			client* clt = server.clients;
			while (clt) {
				if (FD_ISSET(clt->fd, &reads)) {
					ssize_t received = recv(clt->fd, recv_buffer, BUFFER_SIZE - 1, MSG_DONTWAIT);
					if (received < 0)
						return exit_fatal(&server, sockfd);
					else if (received == 0) {
						size_t sender = clt->id;
						client* next = clean_client(clt);
						if (previous) {
							previous->next = next;
							size_t length = sprintf(buffer, "server: client %ld just left\n", sender);
							if (broadcast(&server, sender, buffer, length))
								return exit_fatal(&server, sockfd);
						}
						else
							server.clients = NULL;
						clt = next;
					}
					else {
						recv_buffer[received] = 0;
						ssize_t offset = 0;
						char* line = NULL;
						while (offset < received) {
							int extracted = extract_message(recv_buffer, &line);
							if (extracted < 0)
								return exit_fatal(&server, sockfd);
							else if (extracted > 0) {
								size_t length = sprintf(buffer, "client %ld: %s", clt->id, line);
								offset += strlen(line);
								free(line);
								if (broadcast(&server, clt->id, buffer, length))
									return exit_fatal(&server, sockfd);
							}
							else {
								if (!clt->buffer)
									clt->buffer = line;
								else {
									char* merged = str_join(clt->buffer, line);
									clt->buffer = merged;
									free(line);
								}
								offset = received;
							}
						}
						previous = clt;
						clt = clt->next;
					}
				}
				else if (FD_ISSET(clt->fd, &writes) && clt->queue) {
					message* msg = clt->queue;
					ssize_t sent = send(clt->fd, msg->content + msg->offset, msg->length - msg->offset, MSG_DONTWAIT);
					if (msg->offset + sent < msg->length)
						msg->offset += sent;
					else {
						message* next = msg->next;
						free(msg->content);
						free(msg);
						clt->queue = next;
					}
					previous = clt;
					clt = clt->next;
				}
				else {
					previous = clt;
					clt = clt->next;
				}
			}
		}
	}

	return clean_exit(&server, sockfd, 0);
}
