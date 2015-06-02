//============================================================================
// Name        : gpio-irq-test.cpp
// Author      : Black Swift team
// Version     :
// Copyright   : (c) Black Swift team <info@black-swift.com>
// Description : Test application for the gpio-irq-handler module
//============================================================================

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

//#define SIG_GPIO_IRQ	(SIGRTMIN+10)	// SIGRTMIN is different in Kernel and User modes
#define SIG_GPIO_IRQ	42				// So we have to hardcode this value

////////////////////////////////////////////////////////////////////////////////

void irq_handler(int n, siginfo_t *info, void *unused)
{
	printf("Received value 0x%X\n", info->si_int);
}

////////////////////////////////////////////////////////////////////////////////

bool init_handler(int gpio)
{
	int fd;
	char buf[100];

	struct sigaction sig;
	sig.sa_sigaction=irq_handler;
	sig.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigaction(SIG_GPIO_IRQ, &sig, NULL);

	fd=open("/sys/kernel/debug/gpio-irq", O_WRONLY);
	if(fd < 0)
	{
		perror("open");
		return false;
	}

	sprintf(buf, "+ %d %i", gpio, getpid());

	if(write(fd, buf, strlen(buf) + 1) < 0)
	{
		perror("write");
		close(fd);
		return false;
	}

	close(fd);
	return true;
}

////////////////////////////////////////////////////////////////////////////////

bool remove_handler(int gpio)
{
	int fd;
	char buf[100];

	fd=open("/sys/kernel/debug/gpio-irq", O_WRONLY);
	if(fd < 0)
	{
		perror("open");
		return false;
	}

	sprintf(buf, "- %d %i", gpio, getpid());

	if(write(fd, buf, strlen(buf) + 1) < 0)
	{
		perror("write");
		close(fd);
		return false;
	}

	close(fd);
	return true;
}

////////////////////////////////////////////////////////////////////////////////

int main(int argc, char* argv[])
{
	if(argc > 1)
	 {
		int gpio=atoi(argv[1]);

		if(!init_handler(gpio))
		{
			printf("Can't handle GPIO %d!\n",gpio);
			return -1;
		}

		sigset_t sigset;
	    siginfo_t siginfo;

	    sigemptyset(&sigset);
	    sigaddset(&sigset, SIGINT);	//	Ctrl+C
//		sigaddset(&sigset, SIGQUIT);
//		sigaddset(&sigset, SIGSTOP);
//	    sigaddset(&sigset, SIGTERM);

	    sigprocmask(SIG_BLOCK, &sigset, NULL);

		printf("Waiting GPIO %d signals...\n",gpio);

		while(1)
		{
			sigwaitinfo(&sigset, &siginfo);

			if(siginfo.si_signo == SIGINT)
			{
				remove_handler(gpio);
				return 0;
			}
		}
	}

	printf("Need GPIO number to handle.\n");
	return -1;
}

////////////////////////////////////////////////////////////////////////////////
