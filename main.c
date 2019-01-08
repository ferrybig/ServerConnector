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
#include <fcntl.h>
#include <netinet/in.h>

#define    print_err(format, args...)   printf("[%s:%d][err]" format "\n", __func__, __LINE__, ##args)
#define    print_info(format, args...)   printf("[%s:%d][info]" format "\n", __func__, __LINE__, ##args)
#define PORT 2000


typedef struct State {
	bool isQuiting;
	pid_t processId;
	int startTimeout;
	int fdToServer;
	int fdFromServer ;
	bool locked;
} state;

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
		while ((dup2(fromServer[1], STDOUT_FILENO) == -1) && (errno == EINTR)) {}
		while ((dup2(fromServer[1], STDERR_FILENO) == -1) && (errno == EINTR)) {}
		close(fromServer[1]);
		close(fromServer[0]);
		while ((dup2(toServer[0], STDIN_FILENO) == -1) && (errno == EINTR)) {}
		close(toServer[1]);
		close(toServer[0]);
		chdir("/home/fernando/Documents/mc");
		execl("/bin/sh", "/bin/sh", "-c", "java -jar server.jar nogui", (char*)0);
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
int sgetline(int fd, char ** out)
{
    int buf_size = 0;
    int in_buf = 0;
    int ret;
    char ch;
    char * buffer = NULL;
    char * new_buffer;

    do
    {
        // read a single byte
        ret = read(fd, &ch, 1);
        if (ret < 1)
        {
            // error or disconnect
            free(buffer);
            return -1;
        }

        // has end of line been reached?
        if (ch == '\n')
            break; // yes

        // is more memory needed?
        if ((buf_size == 0) || (in_buf == buf_size))
        {
            buf_size += 128;
            new_buffer = realloc(buffer, buf_size);

            if (!new_buffer)
            {
                free(buffer);
                return -1;
            }

            buffer = new_buffer;
        }

        buffer[in_buf] = ch;
        ++in_buf;
    }
    while (true);

    // if the line was terminated by "\r\n", ignore the
    // "\r". the "\n" is not in the buffer
    if ((in_buf > 0) && (buffer[in_buf-1] == '\r'))
        --in_buf;

    // is more memory needed?
    if ((buf_size == 0) || (in_buf == buf_size))
    {
        ++buf_size;
        new_buffer = realloc(buffer, buf_size);

        if (!new_buffer)
        {
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

int main(int argc, char** argv) {
	state state;
	// Init states
	state.isQuiting = false;
	state.startTimeout = 1;
	state.fdToServer = -1;
	state.fdFromServer = -1;
	state.processId = 0;
	state.locked = false;
	int connections [5] = { -1, -1, -1, -1, -1 };
	int connectionsLength = 5;
	int openServerSockets [1] = { -1 };
	int openServerSocketsLength = 1;

	int opt = 1;
	struct sockaddr_in address;
	int addrlen = sizeof(address);
	int server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(server_fd == 0) {
		perror("Socket failed");
		exit(EXIT_FAILURE);
	}
	// Forcefully attaching socket to the port 8080
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                                                  &opt, sizeof(opt)))
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons( PORT );

    // Forcefully attaching socket to the port 8080
    if (bind(server_fd, (struct sockaddr *)&address,
                                 sizeof(address))<0)
    {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 3) < 0)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }

	openServerSockets[0] = server_fd;

	bool done = false;

	while(!done) {
		print_info("Loop! %d", state.locked);
		int maxfd = -1;
		fd_set fds;
		// Collect the list of file descriptors
		{
			FD_ZERO(&fds); // Clear FD set for select
			if(state.fdFromServer != -1) {
				FD_SET(state.fdFromServer, &fds);
				maxfd = state.fdFromServer;
			}
			bool hasFreeConnectionSlot = false;
			for(int i = 0; i < connectionsLength; i++) {
				if(connections[i] != -1) {
					FD_SET(connections[i], &fds);
					if(connections[i] > maxfd) {
						maxfd = connections[i];
					}
				} else {
					hasFreeConnectionSlot = true;
				}
			}
			if(hasFreeConnectionSlot) {
				for(int i = 0; i < openServerSocketsLength; i++) {
					if(openServerSockets[i] != -1) {
						FD_SET(openServerSockets[i], &fds);
						if(openServerSockets[i] > maxfd) {
							maxfd = openServerSockets[i];
						}
					}
				}
			}
		}
		// Select
		struct timeval tv = {2, 0};
		int code = select(maxfd + 1, &fds, NULL, NULL, &tv);
		if (code == -1) {
			perror("select");
		}
		// Process data
		for(int i = 0; i < connectionsLength; i++) {
			if(connections[i] != -1) {
				if(FD_ISSET(connections[i], &fds)) {
					print_info("Connection %d has data!", i);
					char* line;
					int read = sgetline(connections[i], &line);
					print_info("Read %d byte from sockets!, line: %s", read, line);
					if (read == -1) {
						print_info("EOF on connection %d", i);
						if(shutdown(connections[i], 2) == -1) {
							perror("socket_shutdown");
						}
						if(close(connections[i]) == -1) {
							perror("socket_close");
						}
						connections[i] = -1;
					} else {
						char command = line[0];
						if(command == 'c') { // command
							memmove(line, line + 1, read);
							line[read - 1] = '\n';
							if (state.fdToServer != -1) {
								write(state.fdToServer, line, read);
							} else {
								print_info("Server not running!");
							}
						} else if(command == 'b') { // boot server
							if (state.fdToServer == -1) {
								state.startTimeout = 1;
							} else {
								print_info("Server already running!");
							}
						} else if(command == 's') { // stop
							if (state.fdToServer != -1) {
								state.isQuiting = true;
								state.startTimeout = 0;
							} else {
								print_info("Server already running!");
							}
						} else if(command == 't') { // terminate
							if (state.fdToServer != -1) {
								if(kill(state.processId, SIGTERM) == -1) {
									perror("kill");
								}
							} else {
								print_info("Server not running!");
							}
						} else if(command == 'e') { // exit
							state.locked = true;
							if (state.fdToServer != -1) {
								state.isQuiting = true;
								state.startTimeout = 0;
							}
						}
					}
					free(line);
					
				}
			}
		}
		if(state.fdFromServer != -1) {
			if(FD_ISSET(state.fdFromServer, &fds)) {
				char buf[1024];
				int readLen = read(state.fdFromServer, buf, sizeof(buf));
				if(readLen < sizeof(buf)) {
					buf[readLen] = 0;
				}
				print_info("Read %d bytes from server!, %s", readLen, buf);
				if(readLen == 0) {
					print_info("EOF on server");

					close(state.fdToServer);
					close(state.fdFromServer);
					state.fdToServer = -1;
					state.fdFromServer = -1;
					int status;
					if ( waitpid(state.processId, &status, 0) == -1 ) {
						perror("waitpid() failed");
					} else {
						print_info("Exit code: %d", status);
					}
					state.processId = -1;
					if(state.isQuiting) {
						state.isQuiting = false;
					} else {
						// Automatic restart
						state.startTimeout = 5;
					}
				} else {
					for(int i = 0; i < connectionsLength; i++) {
						if(connections[i] != -1) {
							send(connections[i], buf, readLen, 0);
						}
					}
				}
			}
		}
		for(int i = 0; i < openServerSocketsLength; i++) {
			if(openServerSockets[i] != -1) {
				if(FD_ISSET(openServerSockets[i], &fds)) {
					print_info("Server socket %d has data!", i);
					int newSocket = accept(openServerSockets[i], (struct sockaddr *)&address, (socklen_t*)&addrlen);
					if(newSocket < 0) {
						perror("accept");
					} else {
						for(int i = 0; i < connectionsLength; i++) {
							if(connections[i] == -1) {
								connections[i] = newSocket;
								break;
							}
						}
					}
				}
			}
		}
		// Check server
		if(state.startTimeout > 0 && !state.locked) {
			state.startTimeout--;
			if(state.startTimeout == 0 && !state.isQuiting) {
				startSubprocess(&state);
			}
		}

		done = state.locked && (state.fdToServer == -1);
	}
	// Cleanup
	print_info("Cleanup! %d %d", state.locked, (state.fdToServer == -1));

	for(int i = 0; i < connectionsLength; i++) {
		if(connections[i] != -1) {
			if(close(connections[i]) == -1) {
				perror('close');
			}
		}
	}
	for(int i = 0; i < openServerSocketsLength; i++) {
		if(openServerSockets[i] != -1) {
			if(close(openServerSockets[i]) == -1) {
				perror('close');
			}
		}
	}
	print_info("Cleanup done!");

	return (EXIT_SUCCESS);
}

