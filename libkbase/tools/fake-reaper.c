// SPDX-License-Identifier: MIT
// fake-reaper: `reaper SteamLaunch AppId=N -- prog args...` -> exec prog as a child, so the process ancestry looks like a Steam game launch
// (gamescope derives a client's appId from a "reaper SteamLaunch AppId=" ancestor). Build: cc-aarch64-glibc.
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	int i = 1;
	while (i < argc && strcmp(argv[i], "--")) i++;
	if (i + 1 >= argc) { fprintf(stderr, "usage: reaper SteamLaunch AppId=N -- prog [args]\n"); return 2; }
	pid_t p = fork();
	if (!p) { execvp(argv[i + 1], argv + i + 1); perror("execvp"); _exit(127); }
	int st = 0; waitpid(p, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
