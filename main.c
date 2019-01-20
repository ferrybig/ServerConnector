/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/* 
 * File:   main.c
 * Author: fernando
 *
 * Created on January 6, 2019, 2:01 PM
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <signal.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "./ini.h"
#include "./pack.h"

#define    print_err(format, args...)   printf("[%s:%d][err]" format "\n", __func__, __LINE__, ##args)
#define    print_info(format, args...)   printf("[%s:%d][info]" format "\n", __func__, __LINE__, ##args)
#define PORT 2000

static volatile int signalTerminateSeen = 0;

void signalHandler(int signal) {
	if (signal == SIGINT) { // Console ctrl + c
		signalTerminateSeen = 1;
	}
	if (signal == SIGTERM) { // Terminate
		signalTerminateSeen = 1;
	}
}

int8_t PACKET_STATE_ID = (int8_t) 1;
char* PACKET_STATE_FORMAT = "cs";
int8_t PACKET_OUTPUT_ID = (int8_t) 2;
char* PACKET_OUTPUT_FORMAT = "cs";

typedef struct Configuration {
	int autostart;
	int version;
	bool invalid;
	int wrapper_port;
	char* name;
	char* directory;
	char* cmdline;
} configuration;

static int config_handler(void* user, const char* section, const char* name,
		const char* value) {
	configuration* pconfig = (configuration*) user;

#define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
	if (MATCH("", "version")) {
		pconfig->version = atoi(value);
	} else if (MATCH("", "autostart")) {
		pconfig->autostart = atoi(value);
	} else if (MATCH("", "wrapper_port")) {
		pconfig->wrapper_port = atoi(value);
	} else if (MATCH("", "name")) {
		pconfig->name = strdup(value);
	} else if (MATCH("", "directory")) {
		pconfig->directory = strdup(value);
	} else if (MATCH("", "cmdline")) {
		pconfig->cmdline = strdup(value);
	} else {
		return 0; /* unknown section/name, error */
	}
	return 1;
}

configuration loadConfig(char * file, bool shutdown) {
	configuration config;
	config.autostart = 0;
	config.version = 0;
	config.wrapper_port = 0;
	config.name = "";
	config.directory = "";
	config.cmdline = "";
	config.invalid = false;
	if (ini_parse(file, config_handler, &config) < 0) {
		perror("config");
		if (shutdown) {
			exit(1);
		}
	}
	return config;
}

typedef struct State {
	bool isQuiting;
	pid_t processId;
	int startTimeout;
	int fdToServer;
	int fdFromServer;
	bool locked;
	configuration configuration;
	int connections [5];
	int connectionsLength;
	int openServerSockets [1];
	int openServerSocketsLength;
} state;

state makeState(configuration config) {
	state state = {
		false,
		-1,
		config.autostart,
		-1,
		-1,
		false,
		config,
		{ -1, -1, -1, -1, -1},
		-1,
		{ -1},
		-1,
	};
	state.connectionsLength = sizeof (state.connections) / sizeof (state.connections[0]);
	state.openServerSocketsLength = sizeof (state.openServerSockets) / sizeof (state.openServerSockets[0]);
	return state;
}

int sendState(int fd, state* state) {
	char string[64];
	if (state->fdToServer == -1) {
		// Server not running now
		if (state->startTimeout > 0) {
			strcpy(string, "Starting");
		} else {
			strcpy(string, "Stopped");
		}
	} else {
		// server running at the moment
		if (state->locked == true) {
			strcpy(string, "shutting down");
		} else if (state->isQuiting) {
			strcpy(string, "stopping");
		} else {
			strcpy(string, "running");
		}
	}
	unsigned char buf[1024];
	int packetsize = pack(buf, PACKET_STATE_FORMAT, PACKET_STATE_ID, string);
	return send(fd, buf, packetsize, 0);
}

void sendStateAll(state* state) {
	for (int i = 0; i < state->connectionsLength; i++) {
		if (state->connections[i] != -1) {
			if (sendState(state->connections[i], state) == -1) {
				perror("sendState");
				close(state->connections[i]);
				state->connections[i] = -1;
			}
		}
	}
}

int startSubprocess(state *state) {
	int fromServer[2];
	if (pipe(fromServer) == -1) {
		perror("pipe");
		exit(1);
	}
	int toServer[2];
	if (pipe(toServer) == -1) {
		perror("pipe");
		exit(1);
	}

	pid_t pid = fork();
	if (pid == -1) {
		perror("fork");
		exit(1);
	} else if (pid == 0) {
		while ((dup2(fromServer[1], STDOUT_FILENO) == -1) && (errno == EINTR)) {
		}
		while ((dup2(fromServer[1], STDERR_FILENO) == -1) && (errno == EINTR)) {
		}
		close(fromServer[1]);
		close(fromServer[0]);
		while ((dup2(toServer[0], STDIN_FILENO) == -1) && (errno == EINTR)) {
		}
		close(toServer[1]);
		close(toServer[0]);
		if (chdir(state->configuration.directory) == -1) {
			perror("chdir");
			exit(1);
		}
		execl("/bin/sh", "/bin/sh", "-c", state->configuration.cmdline, (char*) 0);
		perror("execl");
		_exit(1);
	}
	close(fromServer[1]);
	close(toServer[0]);
	state->fdToServer = toServer[1];
	state->fdFromServer = fromServer[0];
	fcntl(state->fdToServer, F_SETFL, O_NONBLOCK);
	state->processId = pid;
}

// Method from http://stackoverflow.com/a/9830092 by Remy Lebeau

int sgetline(int fd, char ** out) {
	int buf_size = 0;
	int in_buf = 0;
	int ret;
	char ch;
	char * buffer = NULL;
	char * new_buffer;

	do {
		// read a single byte
		ret = read(fd, &ch, 1);
		if (ret < 1) {
			// error or disconnect
			free(buffer);
			return -1;
		}

		// has end of line been reached?
		if (ch == '\n')
			break; // yes

		// is more memory needed?
		if ((buf_size == 0) || (in_buf == buf_size)) {
			buf_size += 128;
			new_buffer = realloc(buffer, buf_size);

			if (!new_buffer) {
				free(buffer);
				return -1;
			}

			buffer = new_buffer;
		}

		buffer[in_buf] = ch;
		++in_buf;
	} while (true);

	// if the line was terminated by "\r\n", ignore the
	// "\r". the "\n" is not in the buffer
	if ((in_buf > 0) && (buffer[in_buf - 1] == '\r'))
		--in_buf;

	// is more memory needed?
	if ((buf_size == 0) || (in_buf == buf_size)) {
		++buf_size;
		new_buffer = realloc(buffer, buf_size);

		if (!new_buffer) {
			free(buffer);
			return -1;
		}

		buffer = new_buffer;
	}

	// add a null terminator
	buffer[in_buf] = '\0';

	*out = buffer; // complete line

	return in_buf; // number of chars in the line, not counting the line break and null terminator
}

bool server_is_running(state * state) {
	return state->fdToServer != -1 || state->startTimeout != 0;
}

void server_send_command(state * state, char * command) {
	if (state->fdToServer != -1) {
		if (write(state->fdToServer, command, strlen(command)) == -1) {
			perror("server_send_command: write");
		}
		if (write(state->fdToServer, "\n", 1) == -1) {
			perror("server_send_command: write");
		}
	}
}

void server_start(state * state) {
	if (state->fdToServer == -1 && state->startTimeout == 0) {
		state->startTimeout = 1;
		sendStateAll(state);
	}
}

void server_stop(state * state) {
	if (state->fdToServer != -1 || state->startTimeout != 0) {
		state->isQuiting = true;
		state->startTimeout = 0;
		if (state->fdToServer != -1) {
			server_send_command(state, "stop");
		}
		sendStateAll(state);
	}
}

void server_terminate(state * state) {
	if (server_is_running(state)) {
		state->isQuiting = true;
		state->startTimeout = 0;
		if (state->processId != -1) {
			if (kill(state->processId, SIGTERM) == -1) {
				perror("kill");
			}
		}
	}
}

void server_program_exit(state * state) {
	server_stop(state);
	state->locked = true;
}

bool receiveData(int fd, state * state, int i) {
	char* line;
	int read = sgetline(fd, &line);
	if (read == -1) {
		print_info("EOF on connection %d", i);
		if (shutdown(fd, 2) == -1) {
			perror("socket_shutdown");
		}
		if (close(fd) == -1) {
			perror("socket_close");
		}
		return false;
	} else {
		print_info("Read %d byte from sockets!, line: %s", read, line);
		char command = line[0];
		if (command == 'c') { // command
			//memmove(line, line + 1, read);
			//line[read - 1] = 0;
			server_send_command(state, line + 1);
		} else if (command == 'b') { // boot server
			server_start(state);
		} else if (command == 's') { // stop
			server_stop(state);
		} else if (command == 't') { // terminate
			server_terminate(state);
		} else if (command == 'e') { // exit
			server_program_exit(state);
		}
	}
	free(line);
	return true;
}

void loop(state* state) {
	bool done = false;

	while (!done) {
		//print_info("Loop! %d", signalTerminateSeen);
		int maxfd = -1;
		fd_set fds;
		// Collect the list of file descriptors
		int freeConnectionSlots = 0;
		{
			FD_ZERO(&fds); // Clear FD set for select
			if (state->fdFromServer != -1) {
				FD_SET(state->fdFromServer, &fds);
				maxfd = state->fdFromServer;
			}
			for (int i = 0; i < state->connectionsLength; i++) {
				if (state->connections[i] != -1) {
					FD_SET(state->connections[i], &fds);
					if (state->connections[i] > maxfd) {
						maxfd = state->connections[i];
					}
				} else {
					freeConnectionSlots += 1;
				}
			}
			if (freeConnectionSlots > 0) {
				for (int i = 0; i < state->openServerSocketsLength; i++) {
					if (state->openServerSockets[i] != -1) {
						FD_SET(state->openServerSockets[i], &fds);
						if (state->openServerSockets[i] > maxfd) {
							maxfd = state->openServerSockets[i];
						}
					}
				}
			}
		}
		// Select
		struct timeval tv = {2, 0};
		int select_result;
select:
		select_result = select(maxfd + 1, &fds, NULL, NULL, &tv);
		if (select_result == -1) {
			if (errno == EINTR) { // Interrupted system call
				print_info("Interrupted system call detected");
				goto select;
			}
			perror("select");
		}
		// Process data
		for (int i = 0; i < state->connectionsLength; i++) {
			if (state->connections[i] != -1 && FD_ISSET(state->connections[i], &fds)) {
				print_info("Connection %d has data!", i);
				if (receiveData(state->connections[i], state, i) == false) {
					print_info("EOF on connection %d!", i);
					state->connections[i] = -1;
				}
			}
		}
		if (state->fdFromServer != -1) {
			if (FD_ISSET(state->fdFromServer, &fds)) {
				char buf[1024];
				int readLen = read(state->fdFromServer, buf, sizeof (buf));
				if (readLen < sizeof (buf)) {
					buf[readLen] = 0;
				}

				print_info("Read %d bytes from server!, %s", readLen, buf);
				if (readLen == 0) {
					print_info("EOF on server");

					close(state->fdToServer);
					close(state->fdFromServer);
					state->fdToServer = -1;
					state->fdFromServer = -1;
					int status;
					if (waitpid(state->processId, &status, 0) == -1) {
						perror("waitpid() failed");
					} else {
						print_info("Exit code: %d", status);
					}
					state->processId = -1;
					if (state->isQuiting) {
						state->isQuiting = false;
					} else {
						// Automatic restart
						state->startTimeout = 5;
					}
					sendStateAll(state);
				} else {

					unsigned char packet_buf[1027];
					int packetsize = pack(packet_buf, PACKET_OUTPUT_FORMAT, PACKET_OUTPUT_ID, buf);
					for (int i = 0; i < state->connectionsLength; i++) {
						if (state->connections[i] != -1) {
							send(state->connections[i], packet_buf, packetsize, 0);
						}
					}
				}
			}
		}
		for (int i = 0; i < state->openServerSocketsLength; i++) {
			if (state->openServerSockets[i] != -1 && FD_ISSET(state->openServerSockets[i], &fds) && freeConnectionSlots > 0) {
				print_info("Server socket %d has data!", i);
				struct sockaddr_in6 address_bound = {0};
				socklen_t len = sizeof (address_bound);
				int newSocket = accept(state->openServerSockets[i], (struct sockaddr *) &address_bound, &len);
				if (newSocket < 0) {
					perror("accept");
				} else {
					char str_addr[INET6_ADDRSTRLEN];

					if (inet_ntop(AF_INET6, &(address_bound.sin6_addr), str_addr, sizeof (str_addr)) == NULL) {
						perror("inet_ntop");
						exit(EXIT_FAILURE);
					}

					print_info("Incoming connection! [%s]:%d", str_addr, address_bound.sin6_port);
					sendState(newSocket, state);
					for (int i = 0; i < state->connectionsLength; i++) {
						if (state->connections[i] == -1) {
							state->connections[i] = newSocket;
							break;
						}
					}
					freeConnectionSlots -= 1;
				}
			}
		}
		// Check signal states
		if (signalTerminateSeen > 0) {
			print_info("Terminate signal caught!");
			signalTerminateSeen = 0;
			server_program_exit(state);
		}
		// Check server
		if (state->startTimeout > 0 && !state->locked) {
			state->startTimeout--;
			if (state->startTimeout == 0 && !state->isQuiting && state->fdFromServer == -1) {
				startSubprocess(state);
				sendStateAll(state);
			}
		}

		done = state->locked && (state->fdToServer == -1);
	}
}

typedef struct ProgramOptions {
	char * configFile;
	char * pidfile;
} programoptions;

int main(int argc, char** argv) {
	// Bootstrap
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	// Init program
	programoptions programoptions = {"server.ini", NULL};
	if (argc > 1) {
		programoptions.configFile = argv[1];
		if (argc > 2) {
			programoptions.pidfile = argv[2];
		}
	}

	state state = makeState(loadConfig(programoptions.configFile, true));

	int opt = 1;
	struct sockaddr_in6 address;
	int server_fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (server_fd == 0) {
		perror("Socket failed");
		exit(EXIT_FAILURE);
	}
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
			&opt, sizeof (opt))) {
		perror("setsockopt");
		exit(EXIT_FAILURE);
	}
	int off = 0;
	if (setsockopt(server_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof (off))) {
		perror("setsockopt");
		exit(EXIT_FAILURE);
	}
	address.sin6_family = AF_INET6;
	address.sin6_addr = in6addr_any;
	address.sin6_port = htons(state.configuration.wrapper_port);

	if (bind(server_fd, (struct sockaddr *) &address, sizeof (address)) < 0) {
		perror("bind failed");
		exit(EXIT_FAILURE);
	}
	if (listen(server_fd, 3) < 0) {
		perror("listen");
		exit(EXIT_FAILURE);
	}

	struct sockaddr_in6 address_bound = {0};
	socklen_t len = sizeof (address_bound);
	char str_addr[INET6_ADDRSTRLEN];

	if (getsockname(server_fd, (struct sockaddr *) &address_bound, &len) < 0) {
		perror("getsockname");
		exit(EXIT_FAILURE);
	}
	if (inet_ntop(AF_INET6, &(address_bound.sin6_addr), str_addr, sizeof (str_addr)) == NULL) {
		perror("inet_ntop");
		exit(EXIT_FAILURE);
	}

	print_info("Running on [%s]:%d", str_addr, htons(address_bound.sin6_port));

	state.openServerSockets[0] = server_fd;

	signal(SIGINT, signalHandler);
	signal(SIGTERM, signalHandler);
	print_info("Server ready");

	// Loop
	loop(&state);

	// Cleanup
	print_info("Cleanup! %d %d", state.locked, (state.fdToServer == -1));

	for (int i = 0; i < state.connectionsLength; i++) {
		if (state.connections[i] != -1) {
			if (sendState(state.connections[i], &state) == -1) {
				perror("close: sendstate");
			}
			if (shutdown(state.connections[i], 2) == -1) {
				perror("close");
			}
			if (close(state.connections[i]) == -1) {
				perror("close");
			}
		}
	}
	for (int i = 0; i < state.openServerSocketsLength; i++) {
		if (state.openServerSockets[i] != -1) {
			if (close(state.openServerSockets[i]) == -1) {
				perror("close");
			}
		}
	}
	free(state.configuration.cmdline);
	free(state.configuration.directory);
	free(state.configuration.name);
	print_info("Cleanup done!");

	return (EXIT_SUCCESS);
}

