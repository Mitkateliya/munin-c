/*
 * Copyright (C) 2013 Steve Schnepp <steve.schnepp@pwkf.org> - All rights reserved.
 * Copyright (C) 2013 Helmut Grohne <helmut@subdivi.de> - All rights reserved.
 * Copyright (C) 2013 Diego Elio Petteno <flameeyes@flameeyes.eu> - All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 */
#include <libgen.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


static const int yes = 1;
static const int no = 0;

static int verbose = 0;
static int extension_stripping = 0;

static char* host = "";
static unsigned short port = 0;
static char* ip_bind_as_str = NULL;
static char* plugin_dir = PLUGINDIR;
static char* spoolfetch_dir = "";
static char* client_ip = NULL;

static int handle_connection();

static void oom_handler() {
	static const char* OOM_MSG = "Out of memory\n";

	/* write w/o return check. we are torched anyway */
	write(STDERR_FILENO, OOM_MSG, sizeof(OOM_MSG)-1);
	abort();
}

/* an allocation bigger than MAX_ALLOC_SIZE is bogus */
#define MAX_ALLOC_SIZE (16 * 1024 * 1024)
static void* xmalloc(size_t size) {
	void* ptr;

	assert(size != 0);
	assert(size < MAX_ALLOC_SIZE);

	ptr = malloc(size);
	if (ptr == NULL) oom_handler();
	return ptr;
}

char* xstrdup(const char* s) {
	char* new_str;

	assert(s != NULL);
	assert(strlen(s) < MAX_ALLOC_SIZE);
	new_str = strdup(s);
	if (new_str == NULL) oom_handler();
	return new_str;
}

static int find_plugin_with_basename(/*@out@*/ char *cmdline,
		const char *plugin_dir, const char *plugin_basename) {
       DIR* dirp = opendir(plugin_dir);
       struct dirent* dp;
       int found = 0;
       size_t plugin_basename_len = strlen(plugin_basename);

       /* Empty cmdline */
       cmdline[0] = '\0';

       while (dirp != NULL && (dp = readdir(dirp)) != NULL) {
               char* plugin_filename = dp->d_name;

               if (plugin_filename[0] == '.') {
                       /* No dotted plugin */
                       continue;
               }

               if (strncmp(plugin_filename, plugin_basename, plugin_basename_len) != 0) {
                       /* Does not start with base */
                       continue;
               }

               if (plugin_filename[plugin_basename_len] != '\0' && plugin_filename[plugin_basename_len] != '.') {
                       /* Does not end the string or start an extension */
                       continue;
               }

               snprintf(cmdline, LINE_MAX, "%s/%s", plugin_dir, plugin_filename);
               if (access(cmdline, X_OK) == 0) {
                       /* Found it */
                       found ++;
                       break;
               }
       }
       closedir(dirp);

       return found;
}

static void setenvvars_system(void);

int main(int argc, char *argv[]) {

	int optch;
	extern int opterr;

	char* buf;

	char format[] = "evd:H:s:l:";

	struct sockaddr_in server;
	struct sockaddr_in client;

	socklen_t client_len = sizeof(client);

	int sock_listen;
	int sock_accept;


	opterr = 1;

	while ((optch = getopt(argc, argv, format)) != -1)
	switch (optch) {
		case 'e':
			extension_stripping ++;
			break;
		case 'v':
			verbose ++;
			break;
		case 'd':
			plugin_dir = xstrdup(optarg);
			break;
		case 'H':
			host = xstrdup(optarg);
			break;
		case 's':
			spoolfetch_dir = xstrdup(optarg);
			break;
		case 'l':
			buf = strtok(optarg, ":");
			if (buf) {
				ip_bind_as_str = xstrdup(optarg);
				port = atoi(strtok(NULL, ":"));
			} else {
				port = atoi(optarg);
			}
			break;
	}

	/* get default hostname if not precised */
	if ('\0' == *host) {
		host = (char *) xmalloc(HOST_NAME_MAX + 1);
		gethostname(host, HOST_NAME_MAX);
	}

	/* Prepare static plugin env vars once for all */
	setenvvars_system();

	if (! port) {
		/* use a 1-shot stdin/stdout */
		client_ip = "-";
		client_len = sizeof(client);
		if(0 == getpeername(STDIN_FILENO, (struct sockaddr*)&client,
					&client_len))
			if(client.sin_family == AF_INET)
				client_ip = inet_ntoa(client.sin_addr);
		return handle_connection();
	}

	/* port is set, listen to this port and
           handle clients, one at a time */

	/* Get a socket for accepting connections. */
	if ((sock_listen = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		return(2);
	}

	/* Bind the socket to the server address. */
	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons(port);

	if (! ip_bind_as_str) {
		server.sin_addr.s_addr = INADDR_ANY;
	} else {
		server.sin_addr.s_addr = inet_addr(ip_bind_as_str);
	}

	if (setsockopt(sock_listen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) { 
		perror("setsockopt");
	}

	if (bind(sock_listen, (struct sockaddr*) &server, sizeof(server)) < 0) {
		return(3);
	}

	/* Listen for connections. Specify the backlog as 1. */
	if (listen(sock_listen, 1) != 0) {
		return(4);
	}

	/* Accept a connection. */
	while ((sock_accept = accept(sock_listen, (struct sockaddr*) &client, &client_len)) != -1) { 
		/* connect the accept socket to stdio */
		if (stdin != stdout) {
			fclose(stdout);
		}
		fclose(stdin);
		dup2(sock_accept, 0);
		dup2(sock_accept, 1);

		/* close socket after dup() */
		close(sock_accept);

		stdin = stdout = fdopen(0, "rb+");

		client_ip = inet_ntoa(client.sin_addr);
		if (handle_connection()) break;
	}

	return 5;
}

/* Setting munin specific vars */
static void setenvvars_system() {
	/* Some locales use "," as decimal separator.
	 * This can mess up a lot of plugins. */
	setenv("LC_ALL", "C", yes);

	/* LC_ALL should be enough, but some plugins don't
	 * follow specs (#1014) */
	setenv("LANG", "C", yes);

	/* PATH should be *very* sane by default. Can be
	 * overrided via config file if needed
	 * (Closes #863 and #1128).  */
	setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", yes);
}

/* Setting munin specific vars */
static void setenvvars_munin() {
	/* munin-node will override this with the IP of the
	 * connecting master */
	if (client_ip && client_ip[0] != '\0') {
		setenv("MUNIN_MASTER_IP", client_ip, no);
	}

	/* Tell plugins about supported capabilities */
	setenv("MUNIN_CAP_MULTIGRAPH", "1", no);

	/* We only have one user, so using a fixed path */
	setenv("MUNIN_PLUGSTATE", "/var/tmp", no);
	setenv("MUNIN_STATEFILE", "/dev/null", no);
}

/* Setting user configured vars */
static void setenvvars_conf() {
	/* TODO - add plugin conf parsing */
}

static int handle_connection() {
	char line[LINE_MAX];

	/* Prepare per connection plugin env vars */
	setenvvars_munin();
	setenvvars_conf();

	printf("# munin node at %s\n", host);
	while (fflush(stdout), fgets(line, LINE_MAX, stdin) != NULL) {
		char* cmd;
		char* arg;

		cmd = strtok(line, " \t\n");
		if(cmd == NULL)
			arg = NULL;
		else
			arg = strtok(NULL, " \t\n");

		if (!cmd || strlen(cmd) == 0) {
			printf("# empty cmd\n");
		} else if (strcmp(cmd, "version") == 0) {
			printf("munin c node version: %s\n", VERSION);
		} else if (strcmp(cmd, "nodes") == 0) {
			printf("%s\n", host);
			printf(".\n");
		} else if (strcmp(cmd, "quit") == 0) {
			return(0);
		} else if (strcmp(cmd, "list") == 0) {
			DIR* dirp = opendir(plugin_dir);
			struct dirent* dp;
			while (dirp != NULL && (dp = readdir(dirp)) != NULL) {
				char cmdline[LINE_MAX];
				char* plugin_filename = dp->d_name;;

				if (plugin_filename[0] == '.') {
					/* No dotted plugin */
					continue;
				}

				snprintf(cmdline, LINE_MAX, "%s/%s", plugin_dir, plugin_filename);
				if (access(cmdline, X_OK) == 0) {
					if(extension_stripping) {
						/* Strip after the last . */
						char *last_dot_idx = strrchr(plugin_filename, '.');
						if (last_dot_idx != NULL) {
							*last_dot_idx = '\0';
						}
					}
					printf("%s ", plugin_filename);
				}
			}
			closedir(dirp);
			putchar('\n');
		} else if (
				strcmp(cmd, "config") == 0 ||
				strcmp(cmd, "fetch") == 0
			) {
			char cmdline[LINE_MAX];
			pid_t pid;
			if(arg == NULL) {
				printf("# no plugin given\n");
				continue;
			}
			if(arg[0] == '.' || strchr(arg, '/')) {
				printf("# invalid plugin character\n");
				continue;
			}
			if (! extension_stripping || find_plugin_with_basename(cmdline, plugin_dir, arg) == 0) {
				/* extension_stripping failed, using the plain method */
				snprintf(cmdline, LINE_MAX, "%s/%s", plugin_dir, arg);
			}
			if (access(cmdline, X_OK) == -1) {
				printf("# unknown plugin: %s\n", arg);
				continue;
			}
			if(0 == (pid = vfork())) {
				execl(cmdline, arg, cmd, NULL);
				/* according to vfork(2) we must use _exit */
				_exit(1);
			} else if(pid < 0) {
				printf("# fork failed\n");
				continue;
			} else {
				waitpid(pid, NULL, 0);
			}
			printf(".\n");
		} else if (strcmp(cmd, "cap") == 0) {
			printf("cap ");
			if ('\0' != *spoolfetch_dir) {
				printf("spool ");
			}
			printf("\n");
		} else if (strcmp(cmd, "spoolfetch") == 0) {
			printf("# not implem yet cmd: %s\n", cmd);
		} else {
			printf("# unknown cmd: %s\n", cmd);
		}
	}

	return 0;
}
