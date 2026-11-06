// Tiny sampling profiler for re3 on goodluckOS. With RE3_PROFILE=1, samples the
// PC 1000 times per CPU-second (in practice limited by the kernel tick);
// `kill -USR2 <pid>` writes /dev/shm/re3prof.txt (sample counts per address
// plus /proc/self/maps) and starts a new sampling window. Without RE3_PROFILE
// it does nothing.
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>

#define MAX_SAMPLES (1 << 20)

static unsigned long *samples;
static volatile unsigned int numSamples;
static volatile sig_atomic_t dumpRequested;

static void
onProf(int, siginfo_t *, void *ctx)
{
	if(numSamples < MAX_SAMPLES)
		samples[numSamples++] = ((ucontext_t *)ctx)->uc_mcontext.arm_pc;
}

static void
onDump(int)
{
	dumpRequested = 1;
}

static int
cmpUL(const void *a, const void *b)
{
	unsigned long x = *(const unsigned long *)a, y = *(const unsigned long *)b;
	return x < y ? -1 : x > y;
}

static void
dump(void)
{
	unsigned int n = numSamples;
	numSamples = MAX_SAMPLES;	// stop sampling while we sort
	FILE *f = fopen("/dev/shm/re3prof.txt", "w");
	if(!f)
		return;
	qsort(samples, n, sizeof(*samples), cmpUL);
	fprintf(f, "samples %u\n", n);
	for(unsigned int i = 0; i < n;){
		unsigned int j = i;
		while(j < n && samples[j] == samples[i])
			j++;
		fprintf(f, "%lx %u\n", samples[i], j - i);
		i = j;
	}
	FILE *m = fopen("/proc/self/maps", "r");
	char line[512];
	while(m && fgets(line, sizeof(line), m))
		if(strstr(line, " r-xp ") || strstr(line, " r-xs "))
			fprintf(f, "map %s", line);
	if(m)
		fclose(m);
	fclose(f);
	numSamples = 0;
}

void
GlosProfilePoll(void)
{
	static bool started;
	if(!started){
		started = true;
		if(!getenv("RE3_PROFILE"))
			return;
		samples = (unsigned long *)malloc(MAX_SAMPLES * sizeof(*samples));
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = onProf;
		sa.sa_flags = SA_SIGINFO | SA_RESTART;
		sigaction(SIGPROF, &sa, nullptr);
		signal(SIGUSR2, onDump);
		struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
		setitimer(ITIMER_PROF, &it, nullptr);
	}
	if(dumpRequested){
		dumpRequested = 0;
		dump();
	}
}
