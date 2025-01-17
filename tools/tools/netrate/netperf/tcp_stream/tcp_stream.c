#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define NETPERF_CMD	"netperf"
#define NETPERF_PATH	"/usr/local/bin/" NETPERF_CMD

struct netperf_child {
	int		pipes[2];
};

static void
usage(const char *cmd)
{
	fprintf(stderr, "%s -H host [-l len_s] [-i instances] [-m msgsz] "
	    "[-r|-s]\n", cmd);
	exit(1);
}

int
main(int argc, char *argv[])
{
	struct netperf_child *instance;
	char len_str[32];
	char *args[32];
	const char *host, *msgsz;
	volatile int ninst, set_minmax = 0;
	int len, ninst_done;
	int opt, i, null_fd;
	volatile int reverse = 0, sfile = 0;
	double result, res_max, res_min, jain;

	host = NULL;
	ninst = 2;
	len = 10;
	msgsz = NULL;

	while ((opt = getopt(argc, argv, "H:i:l:m:rs")) != -1) {
		switch (opt) {
		case 'H':
			host = optarg;
			break;

		case 'i':
			ninst = strtoul(optarg, NULL, 10);
			break;

		case 'l':
			len = strtoul(optarg, NULL, 10);
			break;

		case 'm':
			msgsz = optarg;
			break;

		case 'r':
			reverse = 1;
			sfile = 0;
			break;

		case 's':
			reverse = 0;
			sfile = 1;
			break;

		default:
			usage(argv[0]);
		}
	}
	if (ninst <= 0 || host == NULL || len <= 0)
		usage(argv[0]);

	snprintf(len_str, sizeof(len_str), "%d", len);

	i = 0;
	args[i++] = __DECONST(char *, NETPERF_CMD);
	args[i++] = __DECONST(char *, "-P0");
	args[i++] = __DECONST(char *, "-H");
	args[i++] = __DECONST(char *, host);
	args[i++] = __DECONST(char *, "-l");
	args[i++] = __DECONST(char *, len_str);
	args[i++] = __DECONST(char *, "-t");
	if (reverse)
		args[i++] = __DECONST(char *, "TCP_MAERTS");
	else if (sfile)
		args[i++] = __DECONST(char *, "TCP_SENDFILE");
	else
		args[i++] = __DECONST(char *, "TCP_STREAM");
	if (msgsz != NULL) {
		args[i++] = __DECONST(char *, "--");
		args[i++] = __DECONST(char *, "-m");
		args[i++] = __DECONST(char *, msgsz);
	}
	args[i] = NULL;

	instance = calloc(ninst, sizeof(struct netperf_child));
	if (instance == NULL)
		err(1, "calloc failed");

	null_fd = open("/dev/null", O_RDWR);
	if (null_fd < 0)
		err(1, "open null failed");

	for (i = 0; i < ninst; ++i) {
		if (pipe(instance[i].pipes) < 0)
			err(1, "pipe %dth failed", i);
	}

	for (i = 0; i < ninst; ++i) {
		pid_t pid;

		pid = vfork();
		if (pid == 0) {
			int ret;

			dup2(instance[i].pipes[1], STDOUT_FILENO);
			dup2(null_fd, STDERR_FILENO);
			ret = execv(NETPERF_PATH, args);
			if (ret < 0) {
				warn("execv %d failed", i);
				_exit(1);
			}
			/* Never reached */
			abort();
		} else if (pid < 0) {
			err(1, "vfork %d failed", i);
		}
		close(instance[i].pipes[1]);
		instance[i].pipes[1] = -1;
	}

	ninst_done = 0;
	while (ninst_done < ninst) {
		pid_t pid;

		pid = waitpid(-1, NULL, 0);
		if (pid < 0)
			err(1, "waitpid failed");
		++ninst_done;
	}

	res_max = 0.0;
	res_min = 0.0;
	jain = 0.0;
	result = 0.0;
	for (i = 0; i < ninst; ++i) {
		char line[128];
		FILE *fp;

		fp = fdopen(instance[i].pipes[0], "r");
		if (fp == NULL)
			err(1, "fdopen %dth failed", i);

		while (fgets(line, sizeof(line), fp) != NULL) {
			int n, arg1, arg2, arg3;
			double res, arg4;

			n = sscanf(line, "%d%d%d%lf%lf",
			    &arg1, &arg2, &arg3, &arg4, &res);
			if (n == 5) {
				if (!set_minmax) {
					res_max = res;
					res_min = res;
					set_minmax = 1;
				} else {
					if (res > res_max)
						res_max = res;
					if (res < res_min)
						res_min = res;
				}
				jain += (res * res);
				result += res;
				break;
			}
		}
		fclose(fp);
	}

	jain *= ninst;
	jain = (result * result) / jain;

	printf("%s %.2f Mbps\n", reverse ? "TCP_MAERTS" : "TCP_STREAM", result);
	printf("min/max (jain) %.2f Mbps/%.2f Mbps (%f)\n",
	    res_min, res_max, jain);

	exit(0);
}
