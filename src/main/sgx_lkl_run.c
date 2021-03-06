/*
 * Copyright 2016, 2017, 2018 Imperial College London
 * Copyright 2016, 2017 TU Dresden (under SCONE source code license)
 */

#define _GNU_SOURCE
#define WANT_REAL_ARCH_SYSCALLS
#include <link.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <elf.h>
#include <signal.h>
#include <assert.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <linux/if.h>
#include <linux/if_tun.h>

#include "enclave_config.h"
#include "load_elf.h"
#include "mpmc_queue.h"
#include "ring_buff.h"
#include "sgxlkl_util.h"

#include "lkl/linux/virtio_net.h"

#ifdef SGXLKL_HW
#include "enclave_signal.h"
#endif /* SGXLKL_HW */

#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0
#endif
#ifndef str
#define str(X) #X
#endif
#define __merge(a, b) a(b)
#define __stringify(X) __merge(str, X)

extern char __sgxlklrun_text_segment_start;

#if DEBUG

#include "sgxlkl_host_debug.h"

#define MAX_SYSCALL_NUMBER 512
#define MAX_EXIT_REASON_NUMBER 16

static unsigned long _enclave_exit_stats[MAX_EXIT_REASON_NUMBER] = {0};
static const char* const _enclave_exit_reasons[] = {
    "TERMINATE",
    "SYSCALL",
    "ERROR",
    "SLEEP",
    "CPUID",
    "DORESUME"
};

static unsigned long _host_syscall_stats[MAX_SYSCALL_NUMBER];
static int _sigint_handling = 0;

extern unsigned long hw_exceptions;
#endif /* DEBUG */

// One first empty block for bootloaders, and offset in second block
#define EXT4_MAGIC_OFFSET (1024 + 0x38)

static const char* DEFAULT_IPV4_ADDR = "10.0.1.1";
static const char* DEFAULT_IPV4_GW = "10.0.1.254";
static const int   DEFAULT_IPV4_MASK = 24;
static const char* DEFAULT_HOSTNAME = "lkl";
/* The default heap size will only be used if no heap size is specified and
 * either we are in simulation mode, or we are in HW mode and a key is provided
 * via SGXLKL_KEY.
 */
static const size_t DEFAULT_HEAP_SIZE = 200 * 1024 * 1024;

// Is set to 1 when terminating the enclave to prevent concurrent threads from
// trying to reenter.
static int __state_exiting = 0;

static pthread_spinlock_t __stdout_print_lock = {0};
static pthread_spinlock_t __stderr_print_lock = {0};

extern void eresume(uint64_t tcs_id);

#ifdef SGXLKL_HW
char* init_sgx();
int   get_tcs_num();
void  enter_enclave(int tcs_id, uint64_t call_id, void* arg, uint64_t* ret);
uint64_t create_enclave_mem(char* p, char* einit_path, int base_zero, void *base_zero_max);
void     enclave_update_heap(void *p, size_t new_heap, char* key_path);

typedef struct {
    int   tcs_id;
    int   call_id;
    void* args;
} args_t;

__thread int my_tcs_id;
#else /* SGXLKL_HW */
/* By default non-PIE Linux binaries expect their text segment to be mapped to
 * address 0x400000. However, we use the first few pages of the enclave heap
 * for the mmap bitmap containing metadata about mapped/unmapped pages.
 * Therefore, we map the enclave at a lower address to ensure that 0x400000 is
 * available when the executable is mapped.
 */
#define SIM_NON_PIE_ENCL_MMAP_OFFSET 0x200000
#endif /* SGXLKL_HW */

#define VERSION "1.0.0"
#ifdef DEBUG
#define DEBUG_INFO " DEBUG"
#else
#define DEBUG_INFO ""
#endif /* DEBUG */
#ifdef SGXLKL_HW
#define SGX_MODE "Hardware Mode"
#else
#define SGX_MODE "Simulation Mode"
#endif /* SGXLKL_HW */

static void version() {
    printf("SGX-LKL version %s %s%s\n", VERSION, SGX_MODE, DEBUG_INFO);
}

static void usage(char* prog) {
    printf("Usage: %s path/to/encl/file/system path/to/executable <args>\n", prog);
    printf("  path/to/encl/file/system: Path to the ext4 enclave file system image.\n");
    printf("  path/to/executable: Path to executable on enclave file system.\n");
    printf("  <args>: Arguments for executable.\n");
    printf("\nSGX-LKL configuration via environment variables:\n");
    printf("## General ##\n");
    printf("SGXLKL_CMDLINE: Linux kernel command line.\n");
    printf("SGXLKL_SIGPIPE: Set to 1 to enable delivery of SIGPIPE.\n");
    printf("SGXLKL_NON_PIE: Set to 1 when running applications not compiled as position-independent. In this case the size of the enclave is limited to the available space at the beginning of the address space.\n");
    printf("\n## Scheduling & Host system calls ##\n");
    printf("SGXLKL_ESLEEP: Sleep timeout in the scheduler (in ns).\n");
    printf("SGXLKL_ESPINS: Number of spins inside scheduler before sleeping begins.\n");
    printf("SGXLKL_ETHREADS: Number of enclave threads.\n");
    printf("SGXLKL_STHREADS: Number of system call threads outside the enclave.\n");
    printf("SGXLKL_MAX_USER_THREADS: Max. number of user-level thread inside the enclave.\n");
    printf("SGXLKL_REAL_TIME_PRIO: Set to 1 to use realtime priority for enclave threads.\n");
    printf("SGXLKL_SSPINS: Number of spins inside host syscall threads before sleeping begins.\n");
    printf("SGXLKL_SSLEEP: Sleep timeout in the syscall threads (in ns).\n");
    printf("SGXLKL_GETTIME_VDSO: Set to 1 to use the host kernel vdso mechanism to handle clock_gettime calls (Default: 1).\n");
    printf("SGXLKL_ETHREADS_AFFINITY: Specifies the CPU core affinity for enclave threads as a comma-separated list of cores to use, e.g. \"0-2,4\".\n");
    printf("SGXLKL_STHREADS_AFFINITY: Specifies the CPU core affinity for system call threads as a comma-separated list of cores to use, e.g. \"0-2,4\".\n");
    printf("\n## Network ##\n");
    printf("SGXLKL_TAP: Tap for LKL to use as a network interface.\n");
    printf("SGXLKL_TAP_OFFLOAD: Set to 1 to enable partial checksum support, TSOv4, TSOv6, and mergeable receive buffers for the TAP interface.\n");
    printf("SGXLKL_TAP_MTU: Sets MTU on the SGX-LKL side of the TAP interface. Must be set on the host separately (e.g. ifconfig sgxlkl_tap0 mtu 9000).\n");
    printf("SGXLKL_IP4: IPv4 address to assign to LKL (Default: %s).\n", DEFAULT_IPV4_ADDR);
    printf("SGXLKL_GW4: IPv4 gateway to assign to LKL (Default: %s).\n", DEFAULT_IPV4_GW);
    printf("SGXLKL_MASK4: CIDR mask for LKL to use (Default: %d).\n", DEFAULT_IPV4_MASK);
    printf("SGXLKL_HOSTNAME: Host name for LKL to use (Default: %s).\n", DEFAULT_HOSTNAME);
    printf("SGXLKL_HOSTNET: Use host network directly without going through the in-enclave network stack.\n");
    printf("\n## Disk ##\n");
    printf("SGXLKL_HD_VERITY: Volume hash for the root file system image.\n");
    printf("SGXLKL_HD_RO: Set to 1 to mount the root file system as read-only.\n");
    printf("SGXLKL_HDS: Secondary file system images. Comma-separated list of the format: disk1path:disk1mntpoint:disk1mode,disk2path:disk2mntpoint:disk2mode,[...].\n");
    printf("\n## Memory ##\n");
    printf("SGXLKL_HEAP: Total heap size (in bytes) available in the enclave. This includes memory used by the kernel.\n");
    printf("SGXLKL_STACK_SIZE: Stack size of in-enclave user-level threads.\n");
    printf("SGXLKL_MMAP_FILE_SUPPORT: <Not yet supported>\n");
    printf("SGXLKL_SHMEM_FILE: Name of the file to be used for shared memory between the enclave and the outside.\n");
    printf("SGXLKL_SHMEM_SIZE: Size of the file to be used for shared memory between the enclave and the outside.\n");
    printf("\n## Debugging ##\n");
    printf("SGXLKL_VERBOSE: Print information about the SGX-LKL start up process as well as kernel messages.\n");
    printf("SGXLKL_TRACE_MMAP: Print detailed information about in-enclave mmap/munmap operations.\n");
    printf("SGXLKL_TRACE_THREAD: Print detailed information about in-enclave user level thread scheduling.\n");
    printf("SGXLKL_TRACE_SYSCALL: Print detailed information about all system calls.\n");
    printf("SGXLKL_TRACE_LKL_SYSCALL: Print detailed information about in-enclave system calls handled by LKL.\n");
    printf("SGXLKL_TRACE_INTERNAL_SYSCALL: Print detailed information about in-enclave system calls not handled by LKL (in particular mmap/mremap/munmap and futex).\n");
    printf("SGXLKL_TRACE_HOST_SYSCALL: Print detailed information about host system calls.\n");
    printf("SGXLKL_PRINT_HOST_SYSCALL_STATS: Print statistics on the number of host system calls and enclave exits.\n");
    printf("SGXLKL_PRINT_APP_RUNTIME: Measure and print total runtime of the application itself excluding the enclave and SGX-LKL startup and shutdown time.\n");
    printf("\n%s --version to print version information.\n", prog);
    printf("%s --help to print this help.\n", prog);
}

void *calloc(size_t nmemb, size_t size);

size_t backoff_maxpause = 100;
size_t backoff_factor = 4000;
__attribute__((noinline)) unsigned backoffslow(unsigned n) {
    const size_t maxbackoff = 800;
    struct timespec ts = {0, 0};
    n = n - backoff_maxpause;
    n = n <= maxbackoff ? n : maxbackoff;
    ts.tv_nsec = backoff_factor*n;
    nanosleep(&ts, NULL);
    return backoff_maxpause + n*2;
}

static inline unsigned backoff(unsigned n) {
    if (n <= backoff_maxpause) {
        __asm__ __volatile__( "pause" : : : "memory" );
        return n + 1;
    } else {
        return backoffslow(n);
    }
}

/* find tls phdr inside the shared library that we dlopened;
   HW version should do this inside the enclave */
struct dliterdata {
    const char *name;
    enclave_config_t *e;
};

static inline void do_syscall(syscall_t *sc) {
    unsigned long ret;
    unsigned long n = sc->syscallno;
    register long r10 __asm__("r10") = sc->arg4;
    register long r8 __asm__("r8") = sc->arg5;
    register long r9 __asm__("r9") = sc->arg6;
    __asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(sc->arg1), "S"(sc->arg2),
            "d"(sc->arg3), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    sc->ret_val = ret;
}

void *host_syscall_thread(void *v) {
    enclave_config_t *conf = v;
    volatile syscall_t *scall = conf->syscallpage;
    pthread_spinlock_t* curr_print_lock = NULL;
    size_t i;
    unsigned s;
    union {void *ptr; size_t i;} u;
    u.ptr = MAP_FAILED;
    while (1) {
        for (s = 0; !mpmc_dequeue(&conf->syscallq, &u.ptr);) {s = backoff(s);}
        i = u.i;

#ifdef DEBUG
        long syscallno = scall[i].syscallno;
        __sync_fetch_and_add(&_host_syscall_stats[syscallno], 1);
#endif /* DEBUG */

        /* Acquire ticket lock if the system call writes to stdout or stderr to prevent mangling of concurrent writes */
        if (scall[i].syscallno == SYS_write) {
            int fd = (int) scall[i].arg1;
            if (fd == STDOUT_FILENO) {
                pthread_spin_lock(&__stdout_print_lock);
                curr_print_lock = &__stdout_print_lock;
            } else if (fd == STDERR_FILENO) {
                pthread_spin_lock(&__stderr_print_lock);
                curr_print_lock = &__stderr_print_lock;
            }
        }

        if (scall[i].syscallno == SYS_clock_gettime) {
            scall[i].ret_val = clock_gettime(scall[i].arg1, (struct timespec *)scall[i].arg2);
            if (scall[i].ret_val != 0) {
                scall[i].ret_val = -errno;
            }
        } else {
            do_syscall((syscall_t*)&scall[i]);
        }

        /* Release ticket lock if previously acquired */
        if (curr_print_lock) {
            pthread_spin_unlock(curr_print_lock);
            curr_print_lock = NULL;
        }

#ifdef DEBUG
        if (getenv_bool("SGXLKL_TRACE_SYSCALL", 0) || getenv_bool("SGXLKL_TRACE_HOST_SYSCALL", 0)) {
            pthread_spin_lock(&__stdout_print_lock);
            log_host_syscall(syscallno, scall[i].ret_val, scall[i].arg1, scall[i].arg2, scall[i].arg3, scall[i].arg4, scall[i].arg5, scall[i].arg6);
            pthread_spin_unlock(&__stdout_print_lock);
        }
#endif /* DEBUG */

        if (scall[i].status == 1) {
            /* This was submitted by the scheduler or a pinned thread, no need to push anything to queue */
            __atomic_store_n(&scall[i].status, 2, __ATOMIC_RELEASE);
        } else {
            for (s = 0; !mpmc_enqueue(&conf->returnq, u.ptr);) {s = backoff(s);}
        }
    }

    return NULL;
}

static int is_disk_encrypted(int fd) {
    unsigned char magic[2] = {0};
    ssize_t read_bytes = pread(fd, magic, 2, EXT4_MAGIC_OFFSET);
    if (read_bytes != 2) {
        perror("pread(disk,2,EXT4_MAGIC_OFFSET)");
        return 0;
    }
    return !(magic[0] == 0x53 && magic[1] == 0xEF);
}

static void register_hd(enclave_config_t* encl, char* path, char* mnt, int readonly) {
    size_t idx = encl->num_disks;

    if (strlen(mnt) > SGXLKL_DISK_MNT_MAX_PATH_LEN) {
        fprintf(stderr, "Error: Mount path for disk %lu too long (maximum length is %d): \"%s\"\n", idx, SGXLKL_DISK_MNT_MAX_PATH_LEN, mnt);
        exit(EXIT_FAILURE);
    }
    if (path == NULL || strlen(path) == 0)
        return;

    int fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "Unable to open disk file %s for %s access: %s\n", path, readonly ? "read" : "read/write", strerror(errno));
        exit(EXIT_FAILURE);
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        perror("fcntl(disk_fd, F_GETFL)");
        exit(EXIT_FAILURE);
    }
    int res = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (res == -1) {
        perror("fcntl(disk_fd, F_SETFL)");
        exit(EXIT_FAILURE);
    }

    encl->disks[idx].fd = fd;
    encl->disks[idx].ro = readonly;
    strncpy(encl->disks[idx].mnt, mnt, SGXLKL_DISK_MNT_MAX_PATH_LEN);
    encl->disks[idx].mnt[SGXLKL_DISK_MNT_MAX_PATH_LEN] = '\0';
    encl->disks[idx].enc = is_disk_encrypted(fd);
    ++encl->num_disks;
}

static void register_hds(enclave_config_t *encl, char *root_hd) {
    // Count disks to register
    size_t num_disks = 1; // Root disk
    char *hds_str = getenv_str("SGXLKL_HDS", "");
    if (hds_str[0]) {
        num_disks++;
        for (int i = 0; hds_str[i]; i++) {
            if (hds_str[i] == ',') num_disks++;
        }
    }

    // Allocate space for encave disk configurations
    encl->disks = (struct enclave_disk_config*) malloc(sizeof(struct enclave_disk_config) * num_disks);
    // Initialize encl->num_disks, will be adjusted by register_hd
    encl->num_disks = 0;
    // Register root disk
    register_hd(encl, root_hd, "/", getenv_bool("SGXLKL_HD_RO", 0));
    // Register secondary disks
    while (*hds_str) {
        char *hd_path = hds_str;
        char *hd_mnt = strchrnul(hd_path, ':');
        *hd_mnt = '\0';
        hd_mnt++;
        char *hd_mnt_end = strchrnul(hd_mnt, ':');
        *hd_mnt_end = '\0';
        int hd_ro = hd_mnt_end[1] == '1' ? 1 : 0;
        register_hd(encl, hd_path, hd_mnt, hd_ro);

        hds_str = strchrnul(hd_mnt_end + 1, ',');
        while(*hds_str == ' ' || *hds_str == ',') hds_str++;
    }
}

static void *register_shm(char* path, size_t len) {
    if (path == NULL || strlen(path) == 0)
        exit(EXIT_FAILURE);

    int fd = shm_open(path, O_TRUNC | O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
    if (fd == -1) {
        fprintf(stderr, "Error: unable to access shared memory %s (%s)\n", path, strerror(errno));
        perror("open()");
        exit(EXIT_FAILURE);
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        perror("fcntl(shmem_fd, F_GETFL)");
        exit(EXIT_FAILURE);
    }
    int res = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (res == -1) {
        perror("fcntl(shmem_fd, F_SETFL)");
        exit(EXIT_FAILURE);
    }

    if (len <= 0) {
        fprintf(stderr, "Error: invalid memory size length %zu\n", len);
        exit(EXIT_FAILURE);
    }

    if(ftruncate(fd, len) == -1) {
        fprintf(stderr, "ftruncate: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    void *addr;
    if ((addr = mmap(0, len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    close(fd);
    return addr;
}

static void register_net(enclave_config_t* encl, const char* tapstr, const char* ip4str,
        const char* mask4str, const char* gw4str, const char* hostname) {
    // Set hostname
    if(hostname) {
        strncpy(encl->hostname, hostname, sizeof(encl->hostname));
    } else {
        strncpy(encl->hostname, DEFAULT_HOSTNAME, sizeof(encl->hostname));
    }
    encl->hostname[sizeof(encl->hostname) - 1] = '\0';

    if (encl->net_fd != 0) {
        fprintf(stderr, "Error: multiple network interfaces not supported yet\n");
        exit(EXIT_FAILURE);
    }

    // Open tap device FD
    if (tapstr == NULL || strlen(tapstr) == 0) {
        if (getenv_bool("SGXLKL_VERBOSE", 0))
            printf("[    SGX-LKL   ] No tap device specified, networking will not be available.\n");
        return;
    }
    struct ifreq ifr;
    strncpy(ifr.ifr_name, tapstr, IFNAMSIZ);
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

    int vnet_hdr_sz = 0;
    if (getenv_bool("SGXLKL_TAP_OFFLOAD", 0)) {
        ifr.ifr_flags |= IFF_VNET_HDR;
        vnet_hdr_sz = sizeof(struct lkl_virtio_net_hdr_v1);
    }

    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        perror("[    SGX-LKL   ] Error: TUN network device unavailable, open(\"/dev/net/tun\") failed");
        exit(EXIT_FAILURE);
    }
    if (ioctl(fd, TUNSETIFF, &ifr) == -1) {
        fprintf(stderr, "[    SGX-LKL   ] Error: Tap device %s unavailable, ioctl(\"/dev/net/tun\"), TUNSETIFF) failed: %s\n", tapstr, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (vnet_hdr_sz && ioctl(fd, TUNSETVNETHDRSZ, &vnet_hdr_sz) != 0) {
        fprintf(stderr, "failed to TUNSETVNETHDRSZ: /dev/net/tun: %s\n", strerror(errno));
        close(fd);
        exit(EXIT_FAILURE);
    }

    int offload_flags = 0;
    if (getenv_bool("SGXLKL_TAP_OFFLOAD", 0)) {
        offload_flags = TUN_F_TSO4 | TUN_F_TSO6 | TUN_F_CSUM;
    }

    if (ioctl(fd, TUNSETOFFLOAD, offload_flags) != 0) {
        fprintf(stderr, "failed to TUNSETOFFLOAD: /dev/net/tun: %s\n", strerror(errno));
        close(fd);
        exit(EXIT_FAILURE);
    }

    // Read IPv4 addr if there is one
    if (ip4str == NULL) {
        ip4str = DEFAULT_IPV4_ADDR;
    }
    struct in_addr ip4 = { 0 };
    if (inet_pton(AF_INET, ip4str, &ip4) != 1) {
        fprintf(stderr, "[    SGX-LKL   ] Error: Invalid IPv4 address %s\n", ip4str);
        exit(EXIT_FAILURE);
    }

    // Read IPv4 gateway if there is one
    if (gw4str == NULL) {
        gw4str = DEFAULT_IPV4_GW;
    }
    struct in_addr gw4 = { 0 };
    if (gw4str != NULL && strlen(gw4str) > 0 &&
            inet_pton(AF_INET, gw4str, &gw4) != 1) {
        fprintf(stderr, "[    SGX-LKL   ] Error: Invalid IPv4 gateway %s\n", ip4str);
        exit(EXIT_FAILURE);
    }

    // Read IPv4 mask str if there is one
    int mask4 = (mask4str == NULL ? DEFAULT_IPV4_MASK : atoi(mask4str));
    if (mask4 < 1 || mask4 > 32) {
        fprintf(stderr, "[    SGX-LKL   ] Error: Invalid IPv4 mask %s\n", mask4str);
        exit(EXIT_FAILURE);
    }

    encl->net_fd = fd;
    encl->net_ip4 = ip4;
    encl->net_gw4 = gw4;
    encl->net_mask4 = mask4;
}

#define MIN(a,b) (((a)<(b))?(a):(b))

void set_sysconf_params(enclave_config_t *conf) {
    long no_ethreads = (long) getenv_uint64("SGXLKL_ETHREADS", 1, 1024);
    conf->sysconf_nproc_conf = MIN(sysconf(_SC_NPROCESSORS_CONF), no_ethreads);
    conf->sysconf_nproc_onln = MIN(sysconf(_SC_NPROCESSORS_ONLN), no_ethreads);

}

/* Parses the string provided as config for CPU affinity specifications. The
 * specification must consist of a comma-separated list of core IDs. It can
 * contain ranges. For example, "0-2,4" is a valid specification.
 * This function will allocate an array of all specified core IDs and stores
 * it's address at **cores. The number of valid array entries is stored at
 * cores_len.
 *
 * The memory allocated for the array should be free'd by the caller.
 */
void parse_cpu_affinity_params(char *config, int **cores, size_t *cores_len) {
    *cores = NULL;
    *cores_len = 0;

    if (!config || !strlen(config)) {
        return;
    }

    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    // For simplicitly we allocate an array of size nproc to hold all cores
    // that are to be used. When the affinity is set to use a subset of cores,
    // *cores_len will reflect this.
    *cores = malloc(sizeof(int)*nproc);

    char *curr_ptr = config;
    long val = 0, range_start = -1;
    while (*curr_ptr && *cores_len < nproc) {
        switch (*curr_ptr) {
        case '0' ... '9': {
            // strtol will advance curr_ptr to the next non-digit character.
            val = strtol(curr_ptr, &curr_ptr, 10);
            if (val < 0 || val >= nproc) {
                fprintf(stderr, "[    SGX-LKL   ] Invalid CPU affinity range: %s, value %lu is larger or equal than the number of available cores (%lu).\n", config, val, nproc);
                return;
            } else if (range_start < 0) {
                (*cores)[(*cores_len)++] = (int) val;
            } else { // Range (range_start has already been added)
                for (; val > range_start && *cores_len < nproc; val--) {
                    (*cores)[(*cores_len)++] = (int) val;
                }
                range_start = -1;
            }
            break;
        }
        case ',':
            curr_ptr++;
            break;
        case '-':
            range_start = val;
            curr_ptr++;
            break;
        default:
            fprintf(stderr, "[    SGX-LKL   ] Invalid CPU affinity range: %s\n", config);
            return;
        }
    }
}

#ifdef SGXLKL_HW
void do_cpuid(unsigned int* reg) {
    __asm__ __volatile__ ("cpuid":\
            "=a" (reg[0]), "=b" (reg[1]), "=c" (reg[2]), "=d" (reg[3]) :\
            "a" (reg[0]), "c"(reg[2]));
}

void* enclave_thread(void* parm) {
    args_t* args = (args_t*)parm;
    uint64_t ret[2];
    int exit_code = 0;
    my_tcs_id = args->tcs_id;
    while (!__state_exiting) {
        enter_enclave(args->tcs_id, args->call_id, args->args, ret);
#ifdef DEBUG
        __sync_fetch_and_add(&_enclave_exit_stats[ret[0]], 1);
#endif /* DEBUG */
        switch (ret[0]) {
            case SGXLKL_EXIT_TERMINATE: {
                __state_exiting = 1;
                exit_code = ret[1];
                exit(exit_code);
            }
            case SGXLKL_EXIT_CPUID: {
                unsigned int* reg = (unsigned int*)ret[1];
                do_cpuid(reg);
                args->call_id = SGXLKL_ENTER_SYSCALL_RESUME;
                break;
            }
            case SGXLKL_EXIT_SLEEP: {
                struct timespec sleep = {0, ret[1]};
                nanosleep(&sleep, NULL);
                args->call_id = SGXLKL_ENTER_SYSCALL_RESUME;
                break;
            }
            case SGXLKL_EXIT_ERROR: {
                fprintf(stderr, "error inside enclave, error code: %lu \n", ret[1]);
                exit(EXIT_FAILURE);
            }
            case SGXLKL_EXIT_DORESUME: {
                eresume(my_tcs_id);
            }
            default:
                fprintf(stderr, "Unexpected exit reason from signal handler.\n");
        }
    }

done:
    return 0;
}
#endif

#ifdef SGXLKL_HW

void forward_signal(int signum, void *handler_arg) {
    uint64_t call_id = SGXLKL_ENTER_HANDLE_SIGNAL;
    uint64_t ret[2];
    void * arg;
    enclave_signal_info_t siginfo;
    siginfo.signum = signum;
    siginfo.arg = handler_arg;
    arg = &siginfo;
reenter:
    if (__state_exiting) return;
    enter_enclave(my_tcs_id, call_id, arg, ret);
#ifdef DEBUG
    __sync_fetch_and_add(&_enclave_exit_stats[ret[0]], 1);
#endif /* DEBUG */
    switch (ret[0]) {
        case SGXLKL_EXIT_CPUID: {
            unsigned int* reg = (unsigned int*)ret[1];
            do_cpuid(reg);
            call_id = SGXLKL_ENTER_SYSCALL_RESUME;
            goto reenter;
        }
        case SGXLKL_EXIT_DORESUME: {
            return;
        }
        case SGXLKL_EXIT_TERMINATE: {
            __state_exiting = 1;
            int exit_code = (int)ret[1];
            exit(exit_code);
        }
        default:
            fprintf(stderr, "Unexpected exit reason from signal handler.\n");
            //TODO: Other exit reasons (possible for SIGSEGV)
    }
}

void sigill_handler(int sig, siginfo_t *si, void *unused) {
    uint64_t low, high;
    uint64_t rsi;
    /* do rdtsc just in case */
    __asm("rdtscp" : "=a"(low), "=d"(high) : : "rcx");
    rsi = (high << 32) + low;
    forward_signal(SIGILL, (void*) rsi);
}

void sigsegv_handler(int sig, siginfo_t *si, void *unused) {
    // Just forward signal
    forward_signal(SIGSEGV, (void*) si);
}
#endif

void __attribute__ ((noinline)) __gdb_hook_starter_ready(enclave_config_t *conf) {
    __asm__ volatile ( "nop" : : "m" (conf) );
}

#ifdef DEBUG
void print_host_syscall_stats() {
    // If we are exiting from the SIGINT handler, we already printed the
    // syscall stats.
    if (_sigint_handling)
        return;

    printf("Enclave exits: \n");
    printf("Calls      Exit reason          No.\n");
    for (int i = 0; i < MAX_EXIT_REASON_NUMBER; i++) {
        if(_enclave_exit_stats[i]) {
            printf("%10lu %20s %d\n", _enclave_exit_stats[i],  (i < sizeof(_enclave_exit_reasons)) ? _enclave_exit_reasons[i] : "UNKNOWN", i);
        }
    }

// Commented out for now as any additional computation in the AEX handler seems
// to lead to deadlocks while running under gdb in HW mode and potentially
// under other circumstances as well.
//    printf("\nHardware exceptions: %lu\n", hw_exceptions);

    printf("\nHost syscalls: \n");
    printf("Calls      Syscall              No.\n");
    for (int i = 0; i < MAX_SYSCALL_NUMBER; i++) {
        if(_host_syscall_stats[i]) {
            printf("%10lu %20s %d\n", _host_syscall_stats[i],  (i < sizeof(_syscall_names)) ? _syscall_names[i] : "UNKNOWN", i);
        }
    }
}

void stats_sigint_handler(int signo) {
        if (_sigint_handling)
            return;

        print_host_syscall_stats();

        _sigint_handling = 1;
        char response[2];
        fprintf(stderr, "\nDo you want to quit (continue execution otherwise)? [y/n]");
        scanf("%1s", &response[0]);
        if (response[0] == 'y' || response[0] == 'Y') {
            exit(EXIT_SUCCESS);
        }

        _sigint_handling = 0;
}
#endif /* DEBUG */

#ifndef DEBUG
void check_debug_envs(char ** envp) {
    const char *dbg_pres[] = {"SGXLKL_TRACE_", "SGXLKL_PRINT_"};

    char envname[128];
    for (char **env = envp; *env != 0; env++) {
        for (int i = 0; i < sizeof(dbg_pres)/sizeof(dbg_pres[0]); i++) {
            if (strncmp(dbg_pres[i], *env, strlen(dbg_pres[i])) == 0) {
                snprintf(envname, MIN(sizeof(envname), strchrnul(*env, '=') - *env + 1), "%s", *env);
                if (getenv_bool(envname, 0)) {
                    fprintf(stderr, "[    SGX-LKL   ] Warning: %s ignored in non-debug mode.\n", envname);
                }
            }
        }
    }
}
#endif /*not DEBUG */

int main(int argc, char *argv[], char *envp[]) {
    void *sq, *rq;
    size_t sqs = 0, rqs = 0;
    size_t ecs = 0;
    size_t ntsyscall = 1;
    size_t ntenclave = 1;
    pthread_t *ts;
    enclave_config_t encl = {0};
    char *root_hd;
    char** auxvp;
    int i, r, rtprio;
    void *_retval;
    int mmapflags = MAP_PRIVATE|MAP_ANONYMOUS;
    int encl_mmap_flags;
    struct sigaction sa;
    pthread_attr_t eattr;
    cpu_set_t set;
    struct sched_param schparam = {0};
    schparam.sched_priority = 10;
    int *ethreads_cores, *sthreads_cores;
    size_t ethreads_cores_len, sthreads_cores_len;

    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        version();
        exit(EXIT_SUCCESS);
    }

    if (argc <= 2 || (argc >= 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))) {
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    root_hd = argv[1];

#ifdef SGXLKL_HW
    encl.mode = SGXLKL_HW_MODE;
#else
    encl.mode = SGXLKL_SIM_MODE;
#endif /* SGXLKL_HW */

    const size_t pagesize = sysconf(_SC_PAGESIZE);
    ecs = sizeof(encl) + (pagesize - (sizeof(encl)%pagesize));
    memset(&sa, 0, sizeof(struct sigaction));

#ifdef DEBUG
    if (getenv_bool("SGXLKL_PRINT_HOST_SYSCALL_STATS", 0)) {
        atexit(&print_host_syscall_stats);
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = stats_sigint_handler;
        if (sigaction(SIGINT, &sa, NULL) == -1) {
            perror("Failed to register SIGINT handler\n");
            return -1;
        }
    }
#else
    /* Print warnings for debug options enabled in non-debug mode. */
    check_debug_envs(envp);
#endif /* DEBUG */

    /* ignore sigpipe? */
    if (getenv_bool("SGXLKL_SIGPIPE", 0) == 0) {
        sa.sa_handler = SIG_IGN;
        sa.sa_flags = 0;
        sigaction(SIGPIPE, &sa, 0);
    }

#ifdef SGXLKL_HW
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = sigill_handler;
    if (sigaction(SIGILL, &sa, NULL) == -1) {
        perror("Failed to register SIGILL handler\n");
        return -1;
    }

    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = sigsegv_handler;
    if (sigaction(SIGSEGV, &sa, NULL) == -1) {
        perror("Failed to register SIGSEGV handler\n");
        return -1;
    }
#endif /* SGXLKL_HW */

    backoff_maxpause = getenv_uint64("SGXLKL_SSPINS", 100, ULONG_MAX);
    backoff_factor = getenv_uint64("SGXLKL_SSLEEP", 4000, ULONG_MAX);

    /* Determine path of libsgxlkl.so (lkl + musl) */
    ssize_t q, pathlen = 1024;
    /* SGXLKL_SO_PATH is relative to path of sgx-lkl-run. */
    char *libsgxlkl_rel = __stringify(SGXLKL_SO_PATH);
    char libsgxlkl[pathlen];
    char *path_sep;

    /* Fallback, look for libsgxlkl.so in directory of sgx-lkl-run */
    if(!*libsgxlkl_rel) {
        libsgxlkl_rel = "libsgxlkl.so";
    }

    memset(libsgxlkl, 0, pathlen);

    q = readlink("/proc/self/exe", libsgxlkl, pathlen);

    if (q <= 0) {
        fprintf(stderr, "Unable to determine path of sgx-lkl-run.\n");
        return -1;
    }
    else if (q > pathlen - strlen(libsgxlkl_rel) - 1) {
        fprintf(stderr, "Provided libsgxlkl.so path is too long.\n");
        return -1;
    }

    path_sep = strrchr (libsgxlkl, '/');
    if (path_sep == NULL)
        return -1;
    strncpy(path_sep + 1, libsgxlkl_rel, pathlen - (path_sep + 1 - libsgxlkl));

    // We need to load this ENV variable quite early (before creation of the first thread)
    encl.stacksize = getenv_uint64("SGXLKL_STACK_SIZE", 512*1024, ULONG_MAX);

#ifndef SGXLKL_HW
    /* initialize heap and system call pages */
    encl.heapsize = getenv_uint64("SGXLKL_HEAP", DEFAULT_HEAP_SIZE, ULONG_MAX);
    encl_mmap_flags = mmapflags;
    if (getenv_bool("SGXLKL_NON_PIE", 0)) {
        if ((char*) SIM_NON_PIE_ENCL_MMAP_OFFSET + encl.heapsize > &__sgxlklrun_text_segment_start) {
            fprintf(stderr, "[    SGX-LKL   ] Error: SGXLKL_HEAP must be smaller than %lu bytes to not overlap with sgx-lkl-run when SGXLKL_NON_PIE is set to 1.\n", (size_t) (&__sgxlklrun_text_segment_start - SIM_NON_PIE_ENCL_MMAP_OFFSET));
            return -1;
        }
        encl_mmap_flags |= MAP_FIXED;
    }
    encl.heap = mmap((void*) SIM_NON_PIE_ENCL_MMAP_OFFSET, encl.heapsize, PROT_EXEC|PROT_READ|PROT_WRITE, encl_mmap_flags, -1, 0);
    if (encl.heap == MAP_FAILED) {
        return -1;
    }
#else
    /* Map enclave file into memory */
    int lkl_lib_fd;
    struct stat lkl_lib_stat;
    if(!(lkl_lib_fd = open(libsgxlkl, O_RDWR))) {
        return -1;
    }

    if(fstat(lkl_lib_fd, &lkl_lib_stat) == -1) {
        return -1;
    }
    char* enclave_start = mmap(0, lkl_lib_stat.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, lkl_lib_fd, 0);

    init_sgx();
    if (getenv("SGXLKL_HEAP") || getenv("SGXLKL_KEY")) {
        if (!getenv("SGXLKL_KEY")) {
            fprintf(stderr, "[    SGX-LKL   ] Error: Heap size but no enclave signing key specified. Please specify a signing key via SGXLKL_KEY.\n");
            return -1;
        }
        enclave_update_heap(enclave_start, getenv_uint64("SGXLKL_HEAP", DEFAULT_HEAP_SIZE, ULONG_MAX), getenv("SGXLKL_KEY"));
    }
    create_enclave_mem(enclave_start, 0, getenv_bool("SGXLKL_NON_PIE", 0), &__sgxlklrun_text_segment_start);
#endif
    encl.maxsyscalls = getenv_uint64("SGXLKL_MAX_USER_THREADS", 256, 100000);

    rqs = sizeof(encl.returnq.buffer)*256;
    sqs = sizeof(encl.syscallq.buffer)*256;
    rq = mmap(0, rqs, PROT_READ|PROT_WRITE, mmapflags, -1, 0);
    sq = mmap(0, sqs, PROT_READ|PROT_WRITE, mmapflags, -1, 0);
    encl.syscallpage = calloc(sizeof(syscall_t), encl.maxsyscalls);
    if (encl.syscallpage == NULL) {
        return -1;
    }

    newmpmcq(&encl.syscallq, sqs, sq);
    newmpmcq(&encl.returnq, rqs, rq);

    encl.vvar = 0;
    if (getenv_bool("SGXLKL_GETTIME_VDSO", 1)) {
        // Retrieve and save vDSO parameters
        void *vdso_base = (char *) getauxval(AT_SYSINFO_EHDR);
        if (vdso_base) {
            encl.vvar = (char *) (vdso_base - 0x3000ULL);
        } else {
            fprintf(stderr, "[    SGX-LKL   ] Warning: No vDSO info in auxiliary vector. vDSO will not be used.\n");
        }
    }

    register_hds(&encl, root_hd);
    register_net(&encl, getenv("SGXLKL_TAP"), getenv("SGXLKL_IP4"), getenv("SGXLKL_MASK4"), getenv("SGXLKL_GW4"), getenv("SGXLKL_HOSTNAME"));

    set_sysconf_params(&encl);
    /*
     * Get shared memory with the outside
     */
    char *shm_file = getenv("SGXLKL_SHMEM_FILE");
    size_t shm_len = getenv_uint64("SGXLKL_SHMEM_SIZE", 0, (size_t) 1024*1024*1024);
    if (shm_file != 0 && strlen(shm_file) > 0 && shm_len > 0) {
        char shm_file_enc_to_out[strlen(shm_file)+4];
        char shm_file_out_to_enc[strlen(shm_file)+4];

        snprintf(shm_file_enc_to_out, strlen(shm_file)+4, "%s-eo", shm_file);
        snprintf(shm_file_out_to_enc, strlen(shm_file)+4, "%s-oe", shm_file);

        encl.shm_common = register_shm(shm_file, shm_len);
        encl.shm_enc_to_out = register_shm(shm_file_enc_to_out, shm_len);
        encl.shm_out_to_enc = register_shm(shm_file_out_to_enc, shm_len);
    }

    /* get system call thread number */
    ntsyscall = getenv_uint64("SGXLKL_STHREADS", 4, 1024);
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    ntenclave = getenv_uint64("SGXLKL_ETHREADS", 1, 1024);
    ts = calloc(sizeof(*ts), ntenclave + ntsyscall);
    if (ts == 0) {
        return -1;
    }

#ifdef SGXLKL_HW
    int num_tcs = get_tcs_num();
    if (num_tcs == 0) {
        fprintf(stderr, "No TCS number specified \n");
        return -1;
    }
    if (num_tcs < ntenclave) {
        fprintf(stderr, "Not enough TCS \n");
        return -1;
    }
#endif

    /* Initialize print spin locks */
    if (pthread_spin_init(&__stdout_print_lock, PTHREAD_PROCESS_PRIVATE) ||
        pthread_spin_init(&__stderr_print_lock, PTHREAD_PROCESS_PRIVATE) ) {
        fprintf(stderr, "Could not initialize print spin locks.\n");
        return -1;
    }

    parse_cpu_affinity_params(getenv("SGXLKL_STHREADS_AFFINITY"), &sthreads_cores, &sthreads_cores_len);
    parse_cpu_affinity_params(getenv("SGXLKL_ETHREADS_AFFINITY"), &ethreads_cores, &ethreads_cores_len);

    /* Launch system call threads */
    for (i = 0; i < ntsyscall; i++) {
        pthread_attr_init(&eattr);
        CPU_ZERO(&set);
        if (sthreads_cores_len) {
            CPU_SET(sthreads_cores[i % sthreads_cores_len] , &set);
        } else {
            CPU_SET(i%nproc, &set);
        }
        pthread_attr_setaffinity_np(&eattr, sizeof(set), &set);
        pthread_create(&ts[i], &eattr, host_syscall_thread, &encl);
        pthread_setname_np(ts[i], "HOST_SYSCALL");
    }

#ifndef SGXLKL_HW
    struct encl_map_info encl_map;
    load_elf(libsgxlkl, &encl_map);
    if(encl_map.base < 0) {
        // Loading liblkl failed.
        fprintf(stderr, "Could not load liblkl.\n");
        return -1;
    }

    encl.base = encl_map.base;
    encl.ifn = encl_map.entry_point;
#endif

    __gdb_hook_starter_ready(&encl);
    encl.argc = argc - 2;
    encl.argv = (argv + 2);

    // Find aux vector (after envp vector)
    for(auxvp = envp; *auxvp; auxvp++);
    encl.auxv = (Elf64_auxv_t*) (++auxvp);

#ifndef SGXLKL_HW
    // Run the relocation routine inside the new environment
    pthread_t init_thread;
    void* continuation_location;
    r = pthread_create(&init_thread, NULL, (void *)encl.ifn, &encl);
    pthread_setname_np(init_thread, "INIT");
    pthread_join(init_thread, &continuation_location);
#endif

    rtprio = getenv_bool("SGXLKL_REAL_TIME_PRIO", 0);
#ifdef SGXLKL_HW
    args_t a[ntenclave];
#endif
    for (i = 0; i < ntenclave; i++) {
        pthread_attr_init(&eattr);
        CPU_ZERO(&set);
        if (ethreads_cores_len) {
            CPU_SET(ethreads_cores[i % ethreads_cores_len] , &set);
        } else {
            CPU_SET(i%nproc, &set);
        }
        pthread_attr_setaffinity_np(&eattr, sizeof(set), &set);

        if (rtprio) {
            pthread_attr_setschedpolicy(&eattr, SCHED_FIFO);
            pthread_attr_setschedparam(&eattr, &schparam);
            pthread_attr_setinheritsched(&eattr, PTHREAD_EXPLICIT_SCHED);
        }
#ifdef SGXLKL_HW
        a[i].call_id = SGXLKL_ENTER_THREAD_CREATE;
        a[i].args = &encl;
        a[i].tcs_id = i;
        r = pthread_create(&ts[ntsyscall + i], &eattr, (void *)enclave_thread, (void *) &a[i]);
#else
        r = pthread_create(&ts[ntsyscall + i], &eattr, (void *)continuation_location, &encl);
#endif
        pthread_setname_np(ts[ntsyscall + i], "ENCLAVE");
        if (rtprio && r == EPERM) {
            printf("%s", "Not allowed to create thread with realtime priority. Exiting. Use\n"
                    "# echo '*         -       rtprio          80' >> /etc/security/limits.conf\n"
                    "and relogin.\n");
            exit(EXIT_FAILURE);
        }
    }
    /* once enclave calls exit(2) we exit anyway, so all threads will never be joined */
    for (i = 0; i < ntsyscall; i++) {
        pthread_join(ts[i], &_retval);
    }

    return 0;
}

