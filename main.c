
/*
 * MAIN.C
 *
 * crond [-l#] [-L logfile | -S ] [-d [#]|-f|-b] [-c crondir] [-s systemdir]
 *
 * run as root, but NOT setuid root
 *
 * Copyright 1994 Matthew Dillon (dillon@apollo.backplane.com)
 * Copyright 2009 James Pryor <profjim@jimpryor.net>
 * May be distributed under the GNU General Public License
 */

#include "defs.h"

Prototype short DebugOpt;
Prototype short LogLevel;
Prototype short ForegroundOpt;
Prototype short LoggerOpt;
Prototype const char *CDir;
Prototype const char *SCDir;
Prototype char  *LogFile;
Prototype uid_t DaemonUid;
Prototype int InSyncFileRoot;

short DebugOpt;
short LogLevel = 8;
short ForegroundOpt = 0;
short LoggerOpt;
const char  *CDir = CRONTABS;
const char  *SCDir = SCRONTABS;
char  *LogFile = LOG_FILE; /* opened with mode 0600 */
uid_t DaemonUid;
int InSyncFileRoot;

int
main(int ac, char **av)
{
	int i;

	/*
	 * parse options
	 */

	DaemonUid = getuid();

	opterr = 0;

	while ((i = getopt(ac,av,"d:l:L:fbSc:s:")) != EOF) {
		switch (i) {
			case 'l':
				{
					LogLevel = atoi(optarg);
				}
				break;
			case 'd':
				DebugOpt = atoi(optarg);
				LogLevel = 0;
				/* fall through to include f too */
			case 'f':
				ForegroundOpt = 1;
				break;
			case 'b':
				ForegroundOpt = 0;
				break;
			case 'S':			/* log through syslog */
				LoggerOpt = 0;
				break;
			case 'L':			/* use internal log formatter */
				LoggerOpt = 1;
				if (*optarg != 0) {
					LogFile = optarg;
				}
				break;
			case 'c':
				if (*optarg != 0) CDir = optarg;
				break;
			case 's':
				if (*optarg != 0) SCDir = optarg;
				break;
			default:
				/*
				 * check for parse error
				 */
				printf("dcron " VERSION "\n");
				printf("crond [-l#] [-L logfile | -S ] [-d [#]|-f|-b] [-c crondir] [-s systemdir]\n");
				printf("-l num\tlogging level (default = 8)\n");
				printf("-L file\tlog to file (default %s)\n-S\tlog to syslogd (default)\n", LOG_FILE);
				printf("-d [num]\tdebugging\n-f\trun in foreground\n-b\trun in background (default)\n");
				printf("-c crondir\tcrontab spool dir (default %s)\n-s systemdir\tsystem cron.d dir (default %s)\n",
						CRONTABS, SCRONTABS);
				exit(2);
		}
	}

	/*
	 * close stdin and stdout.
	 * close unused descriptors -  don't need.
	 * optional detach from controlling terminal
	 */

	fclose(stdin);
	fclose(stdout);
	fclose(stderr);

	i = open("/dev/null", O_RDWR);
	if (i < 0) {
		perror("open: /dev/null:");
		exit(1);
	}
	dup2(i, 0);
	dup2(i, 1);
	dup2(i, 2);

	if (ForegroundOpt == 0) {
		int fd;
		int pid;
		if (setsid() < 0)
			perror("setsid");

		if ((fd = open("/dev/tty", O_RDWR)) >= 0) {
			ioctl(fd, TIOCNOTTY, 0);
			close(fd);
		}

		pid = fork();

		if (pid < 0) {
			perror("fork");
			exit(1);
		}
		if (pid > 0)
			exit(0);
	}

	(void)startlogger();		/* need if syslog mode selected */
	(void)initsignals();		/* set some signal handlers */

	/* 
	 * main loop - synchronize to 1 second after the minute, minimum sleep
	 *             of 1 second.
	 */

	logn(9, "%s " VERSION " ya, started with loglevel %d\n", av[0], LogLevel);
	SynchronizeDir(CDir, NULL, 1);
	SynchronizeDir(SCDir, "root", 1);

	{
		time_t t1 = time(NULL);
		time_t t2;
		long dt;
		short rescan = 60;
		short stime = 60;

		for (;;) {
			sleep((stime + 1) - (short)(time(NULL) % stime));

			t2 = time(NULL);
			dt = t2 - t1;

			/*
			 * The file 'cron.update' is checked to determine new cron
			 * jobs.  The directory is rescanned once an hour to deal
			 * with any screwups.
			 *
			 * check for disparity.  Disparities over an hour either way
			 * result in resynchronization.  A reverse-indexed disparity
			 * less then an hour causes us to effectively sleep until we
			 * match the original time (i.e. no re-execution of jobs that
			 * have just been run).  A forward-indexed disparity less then
			 * an hour causes intermediate jobs to be run, but only once
			 * in the worst case.
			 *
			 * when running jobs, the inequality used is greater but not
			 * equal to t1, and less then or equal to t2.
			 */

			if (--rescan == 0) {
				rescan = 60;
				SynchronizeDir(CDir, NULL, 0);
				SynchronizeDir(SCDir, "root", 0);
			}
			CheckUpdates(CDir, NULL);
			CheckUpdates(SCDir, "root");
			if (DebugOpt)
				logn(5, "Wakeup dt=%d\n", dt);
			if (dt < -60*60 || dt > 60*60) {
				t1 = t2;
				logn(9, "time disparity of %d minutes detected\n", dt / 60);
			} else if (dt > 0) {
				TestJobs(t1, t2);
				RunJobs();
				sleep(5);
				if (CheckJobs() > 0)
					stime = 10;
				else
					stime = 60;
				t1 = t2;
			}
		}
	}
	/* not reached */
}

