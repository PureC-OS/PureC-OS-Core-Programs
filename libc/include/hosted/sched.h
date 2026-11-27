#ifndef PUREC_HOSTED_SCHED_H
#define PUREC_HOSTED_SCHED_H

#ifdef __cplusplus
extern "C"
{
#endif

    struct sched_param
    {
        int sched_priority;
    };

#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2

    typedef struct
    {
        unsigned long bits[16];
    } cpu_set_t;

#define CPU_SETSIZE 1024
#define CPU_ZERO(set) \
    do { for (int cpu_i_ = 0; cpu_i_ < 16; cpu_i_++) (set)->bits[cpu_i_] = 0; } while (0)
#define CPU_SET(cpu, set) ((set)->bits[(cpu) / 64] |= 1UL << ((cpu) % 64))
#define CPU_CLR(cpu, set) ((set)->bits[(cpu) / 64] &= ~(1UL << ((cpu) % 64)))
#define CPU_ISSET(cpu, set) (((set)->bits[(cpu) / 64] >> ((cpu) % 64)) & 1UL)

#define CLONE_NEWTIME 0x00000080
#define CLONE_NEWNS 0x00020000
#define CLONE_NEWCGROUP 0x02000000
#define CLONE_NEWUTS 0x04000000
#define CLONE_NEWIPC 0x08000000
#define CLONE_NEWUSER 0x10000000
#define CLONE_NEWPID 0x20000000
#define CLONE_NEWNET 0x40000000

    int unshare(int flags);
    int setns(int fd, int nstype);
    int clone(int (*function)(void *), void *stack, int flags, void *argument, ...);
    int sched_yield(void);
    int sched_getaffinity(int pid, unsigned long size, cpu_set_t *set);
    int sched_setaffinity(int pid, unsigned long size, const cpu_set_t *set);

#ifdef __cplusplus
}
#endif

#endif
