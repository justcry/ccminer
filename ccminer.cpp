﻿/*
 * Copyright 2010 Jeff Garzik
 * Copyright 2012-2014 pooler
 * Copyright 2014-2015 tpruvot
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "cpuminer-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>

#include <curl/curl.h>
#include <jansson.h>
#include <openssl/sha.h>

#ifdef WIN32
#include <windows.h>
#include <stdint.h>
#else
#include <errno.h>
#include <sys/resource.h>
#if HAVE_SYS_SYSCTL_H
#include <sys/types.h>
#if HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <sys/sysctl.h>
#endif
#endif

#include "miner.h"

#ifdef WIN32
#include <Mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include "compat/winansi.h"
BOOL WINAPI ConsoleHandler(DWORD);
#endif

#define PROGRAM_NAME		"ccminer"
#define LP_SCANTIME		60
#define HEAVYCOIN_BLKHDR_SZ		84
#define MNR_BLKHDR_SZ 80

// from cuda.cpp
int cuda_num_devices();
void cuda_devicenames();
void cuda_shutdown();
int cuda_finddevice(char *name);
void cuda_print_devices();

#include "nvml.h"
#ifdef USE_WRAPNVML
nvml_handle *hnvml = NULL;
#endif

enum workio_commands {
	WC_GET_WORK,
	WC_SUBMIT_WORK,
	WC_ABORT,
};

struct workio_cmd {
	enum workio_commands	cmd;
	struct thr_info		*thr;
	union {
		struct work	*work;
	} u;
};

enum sha_algos {
	ALGO_ANIME,
	ALGO_BLAKE,
	ALGO_BLAKECOIN,
	ALGO_DEEP,
	ALGO_DMD_GR,
	ALGO_DOOM,
	ALGO_FRESH,
	ALGO_FUGUE256,		/* Fugue256 */
	ALGO_GROESTL,
	ALGO_HEAVY,		/* Heavycoin hash */
	ALGO_KECCAK,
	ALGO_JACKPOT,
	ALGO_LUFFA_DOOM,
	ALGO_LYRA2,
	ALGO_MJOLLNIR,		/* Hefty hash */
	ALGO_MYR_GR,
	ALGO_NEOSCRYPT,
	ALGO_NIST5,
	ALGO_PENTABLAKE,
	ALGO_PLUCK,
	ALGO_QUARK,
	ALGO_QUBIT,
	ALGO_SCRYPT,
	ALGO_SCRYPT_JANE,
	ALGO_SKEIN,
	ALGO_SKEIN2,
	ALGO_S3,
	ALGO_X11,
	ALGO_X13,
	ALGO_X14,
	ALGO_X15,
	ALGO_X17,
	ALGO_ZR5,
};

static const char *algo_names[] = {
	"anime",
	"blake",
	"blakecoin",
	"deep",
	"dmd-gr",
	"doom", /* is luffa */
	"fresh",
	"fugue256",
	"groestl",
	"heavy",
	"keccak",
	"jackpot",
	"luffa",
	"lyra2",
	"mjollnir",
	"myr-gr",
	"neoscrypt",
	"nist5",
	"penta",
	"pluck",
	"quark",
	"qubit",
	"scrypt",
	"scrypt-jane",
	"skein",
	"skein2",
	"s3",
	"x11",
	"x13",
	"x14",
	"x15",
	"x17",
	"zr5",
};

bool opt_debug = false;
bool opt_debug_threads = false;
bool opt_protocol = false;
bool opt_benchmark = false;
bool want_longpoll = true;
bool have_longpoll = false;
bool want_stratum = true;
bool have_stratum = false;
bool allow_gbt = true;
bool allow_mininginfo = true;
bool check_dups = false;
static bool submit_old = false;
bool use_syslog = false;
bool use_colors = true;
static bool opt_background = false;
bool opt_quiet = false;
static int opt_retries = -1;
static int opt_fail_pause = 30;
static int opt_time_limit = 0;
static time_t firstwork_time = 0;
int opt_timeout = 270;
static int opt_scantime = 10;
static json_t *opt_config;
static const bool opt_time = true;
static enum sha_algos opt_algo = ALGO_X11;
int opt_n_threads = 0;
int opt_affinity = -1;
int opt_priority = 0;
static double opt_difficulty = 1; // CH
bool opt_extranonce = true;
bool opt_trust_pool = false;
uint16_t opt_vote = 9999;
int num_cpus;
int active_gpus;
char * device_name[MAX_GPUS];
short device_map[MAX_GPUS] = { 0 };
long  device_sm[MAX_GPUS] = { 0 };
uint32_t gpus_intensity[MAX_GPUS] = { 0 };

// un-linked to cmdline scrypt options (useless)
int device_batchsize[MAX_GPUS] = { 0 };
int device_texturecache[MAX_GPUS] = { 0 };
int device_singlememory[MAX_GPUS] = { 0 };
// implemented scrypt options
int parallel = 2; // All should be made on GPU
char *device_config[MAX_GPUS] = { 0 };
int device_backoff[MAX_GPUS] = { 0 };
int device_lookup_gap[MAX_GPUS] = { 0 };
int device_interactive[MAX_GPUS] = { 0 };
int opt_nfactor = 0;
bool opt_autotune = true;
bool abort_flag = false;
char *jane_params = NULL;

// pools (failover/getwork infos)
struct pool_infos pools[MAX_POOLS] = { 0 };
int num_pools = 1;
volatile int cur_pooln = 0;
bool opt_pool_failover = true;
volatile bool pool_is_switching = false;
bool conditional_pool_rotate = false;

// current connection
char *rpc_user = NULL;
static char *rpc_pass;
static char *rpc_userpass = NULL;
char *rpc_url;
static char *short_url = NULL;
struct stratum_ctx stratum = { 0 };

char *opt_cert;
char *opt_proxy;
long opt_proxy_type;
struct thr_info *thr_info;
static int work_thr_id;
struct thr_api *thr_api;
int longpoll_thr_id = -1;
int stratum_thr_id = -1;
int api_thr_id = -1;
bool stratum_need_reset = false;
struct work_restart *work_restart = NULL;
static int app_exit_code = EXIT_CODE_OK;
uint32_t zr5_pok = 0;

pthread_mutex_t applog_lock;
static pthread_mutex_t stats_lock;
static double thr_hashrates[MAX_GPUS] = { 0 };
uint64_t global_hashrate = 0;
double   stratum_diff = 0.0;
double   net_diff = 0;
uint64_t net_hashrate = 0;
uint64_t net_blocks = 0;
// conditional mining
uint8_t conditional_state[MAX_GPUS] = { 0 };
double opt_max_temp = 0.0;
double opt_max_diff = 0.0;
double opt_max_rate = 0.0;

int opt_statsavg = 30;
// strdup on char* to allow a common free() if used
static char* opt_syslog_pfx = strdup(PROGRAM_NAME);
char *opt_api_allow = strdup("127.0.0.1"); /* 0.0.0.0 for all ips */
int opt_api_remote = 0;
int opt_api_listen = 4068; /* 0 to disable */

#ifdef HAVE_GETOPT_LONG
#include <getopt.h>
#else
struct option {
	const char *name;
	int has_arg;
	int *flag;
	int val;
};
#endif

static char const usage[] = "\
Usage: " PROGRAM_NAME " [OPTIONS]\n\
Options:\n\
  -a, --algo=ALGO       specify the hash algorithm to use\n\
			anime       Animecoin\n\
			blake       Blake 256 (SFR)\n\
			blakecoin   Fast Blake 256 (8 rounds)\n\
			deep        Deepcoin\n\
			dmd-gr      Diamond-Groestl\n\
			fresh       Freshcoin (shavite 80)\n\
			fugue256    Fuguecoin\n\
			groestl     Groestlcoin\n\
			heavy       Heavycoin\n\
			jackpot     Jackpot\n\
			keccak      Keccak-256 (Maxcoin)\n\
			luffa       Doomcoin\n\
			lyra2       VertCoin\n\
			mjollnir    Mjollnircoin\n\
			myr-gr      Myriad-Groestl\n\
			neoscrypt   FeatherCoin, Phoenix, UFO...\n\
			nist5       NIST5 (TalkCoin)\n\
			penta       Pentablake hash (5x Blake 512)\n\
			pluck       SupCoin\n\
			quark       Quark\n\
			qubit       Qubit\n\
			scrypt      Scrypt\n\
			scrypt-jane Scrypt-jane Chacha\n\
			skein       Skein SHA2 (Skeincoin)\n\
			skein2      Double Skein (Woodcoin)\n\
			s3          S3 (1Coin)\n\
			x11         X11 (DarkCoin)\n\
			x13         X13 (MaruCoin)\n\
			x14         X14\n\
			x15         X15\n\
			x17         X17\n\
			zr5         ZR5 (ZiftrCoin)\n\
  -d, --devices         Comma separated list of CUDA devices to use.\n\
                        Device IDs start counting from 0! Alternatively takes\n\
                        string names of your cards like gtx780ti or gt640#2\n\
                        (matching 2nd gt640 in the PC)\n\
  -i  --intensity=N[,N] GPU intensity 8.0-25.0 (default: auto) \n\
                        Decimals are allowed for fine tuning \n\
  -f, --diff            Divide difficulty by this factor (std is 1) \n\
  -v, --vote=VOTE       block reward vote (for HeavyCoin)\n\
  -m, --trust-pool      trust the max block reward vote (maxvote) sent by the pool\n\
  -o, --url=URL         URL of mining server\n\
  -O, --userpass=U:P    username:password pair for mining server\n\
  -u, --user=USERNAME   username for mining server\n\
  -p, --pass=PASSWORD   password for mining server\n\
      --cert=FILE       certificate for mining server using SSL\n\
  -x, --proxy=[PROTOCOL://]HOST[:PORT]  connect through a proxy\n\
  -t, --threads=N       number of miner threads (default: number of nVidia GPUs)\n\
  -r, --retries=N       number of times to retry if a network call fails\n\
                          (default: retry indefinitely)\n\
  -R, --retry-pause=N   time to pause between retries, in seconds (default: 30)\n\
      --time-limit      maximum time [s] to mine before exiting the program.\n\
  -T, --timeout=N       network timeout, in seconds (default: 270)\n\
  -s, --scantime=N      upper bound on time spent scanning current work when\n\
                          long polling is unavailable, in seconds (default: 10)\n\
  -n, --ndevs           list cuda devices\n\
  -N, --statsavg        number of samples used to compute hashrate (default: 30)\n\
      --no-gbt          disable getblocktemplate support (height check in solo)\n\
      --no-longpoll     disable X-Long-Polling support\n\
      --no-stratum      disable X-Stratum support\n\
  -q, --quiet           disable per-thread hashmeter output\n\
      --no-color        disable colored output\n\
  -D, --debug           enable debug output\n\
  -P, --protocol-dump   verbose dump of protocol-level activities\n\
      --cpu-affinity    set process affinity to cpu core(s), mask 0x3 for cores 0 and 1\n\
      --cpu-priority    set process priority (default: 3) 0 idle, 2 normal to 5 highest\n\
  -b, --api-bind=port   IP:port for the miner API (default: 127.0.0.1:4068), 0 disabled\n\
      --api-remote      Allow remote control, like pool switching\n\
      --max-temp=N      Only mine if gpu temp is less than specified value\n\
      --max-rate=N[KMG] Only mine if net hashrate is less than specified value\n\
      --max-diff=N      Only mine if net difficulty is less than specified value\n"
#ifdef HAVE_SYSLOG_H
"\
  -S, --syslog          use system log for output messages\n\
      --syslog-prefix=... allow to change syslog tool name\n"
#endif
"\
  -B, --background      run the miner in the background\n\
      --benchmark       run in offline benchmark mode\n\
      --cputest         debug hashes from cpu algorithms\n\
  -c, --config=FILE     load a JSON-format configuration file\n\
  -V, --version         display version information and exit\n\
  -h, --help            display this help text and exit\n\
";

static char const short_options[] =
#ifdef HAVE_SYSLOG_H
	"S"
#endif
	"a:Bc:i:Dhp:Px:mnqr:R:s:t:T:o:u:O:Vd:f:v:N:b:l:L:";

static struct option const options[] = {
	{ "algo", 1, NULL, 'a' },
	{ "api-bind", 1, NULL, 'b' },
	{ "api-remote", 0, NULL, 1030 },
	{ "background", 0, NULL, 'B' },
	{ "benchmark", 0, NULL, 1005 },
	{ "cert", 1, NULL, 1001 },
	{ "config", 1, NULL, 'c' },
	{ "cputest", 0, NULL, 1006 },
	{ "cpu-affinity", 1, NULL, 1020 },
	{ "cpu-priority", 1, NULL, 1021 },
	{ "debug", 0, NULL, 'D' },
	{ "help", 0, NULL, 'h' },
	{ "intensity", 1, NULL, 'i' },
	{ "ndevs", 0, NULL, 'n' },
	{ "no-color", 0, NULL, 1002 },
	{ "no-gbt", 0, NULL, 1011 },
	{ "no-longpoll", 0, NULL, 1003 },
	{ "no-stratum", 0, NULL, 1007 },
	{ "no-autotune", 0, NULL, 1004 },  // scrypt
	{ "interactive", 1, NULL, 1050 },  // scrypt
	{ "launch-config", 0, NULL, 'l' }, // scrypt
	{ "lookup-gap", 0, NULL, 'L' },    // scrypt
	{ "max-temp", 1, NULL, 1060 },
	{ "max-diff", 1, NULL, 1061 },
	{ "max-rate", 1, NULL, 1062 },
	{ "pass", 1, NULL, 'p' },
	{ "pool-name", 1, NULL, 1100 },     // pool
	{ "pool-removed", 1, NULL, 1101 },  // pool
	{ "pool-scantime", 1, NULL, 1102 }, // pool
	{ "pool-time-limit", 1, NULL, 1108 },
	{ "pool-max-diff", 1, NULL, 1161 }, // pool
	{ "pool-max-rate", 1, NULL, 1162 }, // pool
	{ "protocol-dump", 0, NULL, 'P' },
	{ "proxy", 1, NULL, 'x' },
	{ "quiet", 0, NULL, 'q' },
	{ "retries", 1, NULL, 'r' },
	{ "retry-pause", 1, NULL, 'R' },
	{ "scantime", 1, NULL, 's' },
	{ "statsavg", 1, NULL, 'N' },
#ifdef HAVE_SYSLOG_H
	{ "syslog", 0, NULL, 'S' },
	{ "syslog-prefix", 1, NULL, 1018 },
#endif
	{ "time-limit", 1, NULL, 1008 },
	{ "threads", 1, NULL, 't' },
	{ "vote", 1, NULL, 'v' },
	{ "trust-pool", 0, NULL, 'm' },
	{ "timeout", 1, NULL, 'T' },
	{ "url", 1, NULL, 'o' },
	{ "user", 1, NULL, 'u' },
	{ "userpass", 1, NULL, 'O' },
	{ "version", 0, NULL, 'V' },
	{ "devices", 1, NULL, 'd' },
	{ "diff", 1, NULL, 'f' },
	{ 0, 0, 0, 0 }
};

static char const scrypt_usage[] = "\n\
Scrypt specific options:\n\
  -l, --launch-config   gives the launch configuration for each kernel\n\
                        in a comma separated list, one per device.\n\
  -L, --lookup-gap      Divides the per-hash memory requirement by this factor\n\
                        by storing only every N'th value in the scratchpad.\n\
                        Default is 1.\n\
      --interactive     comma separated list of flags (0/1) specifying\n\
                        which of the CUDA device you need to run at inter-\n\
                        active frame rates (because it drives a display).\n\
      --no-autotune     disable auto-tuning of kernel launch parameters\n\
";

#define CFG_NULL 0
#define CFG_POOL 1
struct opt_config_array {
	int cat;
	const char *name;     // json key
	const char *longname; // global opt name if different
} cfg_array_keys[] = {
	{ CFG_POOL, "url", NULL }, /* let this key first, increment pools */
	{ CFG_POOL, "user", NULL },
	{ CFG_POOL, "pass", NULL },
	{ CFG_POOL, "userpass", NULL },
	{ CFG_POOL, "name", "pool-name" },
	{ CFG_POOL, "scantime", "pool-scantime" },
	{ CFG_POOL, "max-diff", "pool-max-diff" },
	{ CFG_POOL, "max-rate", "pool-max-rate" },
	{ CFG_POOL, "removed",  "pool-removed" },
	{ CFG_POOL, "disabled", "pool-removed" }, // sample alias
	{ CFG_POOL, "time-limit", "pool-time-limit" },
	{ CFG_NULL, NULL, NULL }
};

struct work _ALIGN(64) g_work;
time_t g_work_time;
pthread_mutex_t g_work_lock;


#ifdef __linux /* Linux specific policy and affinity management */
#include <sched.h>
static inline void drop_policy(void) {
	struct sched_param param;
	param.sched_priority = 0;
#ifdef SCHED_IDLE
	if (unlikely(sched_setscheduler(0, SCHED_IDLE, &param) == -1))
#endif
#ifdef SCHED_BATCH
		sched_setscheduler(0, SCHED_BATCH, &param);
#endif
}

static void affine_to_cpu_mask(int id, uint8_t mask) {
	cpu_set_t set;
	CPU_ZERO(&set);
	for (uint8_t i = 0; i < num_cpus; i++) {
		// cpu mask
		if (mask & (1<<i)) { CPU_SET(i, &set); }
	}
	if (id == -1) {
		// process affinity
		sched_setaffinity(0, sizeof(&set), &set);
	} else {
		// thread only
		pthread_setaffinity_np(thr_info[id].pth, sizeof(&set), &set);
	}
}
#elif defined(__FreeBSD__) /* FreeBSD specific policy and affinity management */
#include <sys/cpuset.h>
static inline void drop_policy(void) { }
static void affine_to_cpu_mask(int id, uint8_t mask) {
	cpuset_t set;
	CPU_ZERO(&set);
	for (uint8_t i = 0; i < num_cpus; i++) {
		if (mask & (1<<i)) CPU_SET(i, &set);
	}
	cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof(cpuset_t), &set);
}
#else /* Windows */
static inline void drop_policy(void) { }
static void affine_to_cpu_mask(int id, uint8_t mask) {
	if (id == -1)
		SetProcessAffinityMask(GetCurrentProcess(), mask);
	else
		SetThreadAffinityMask(GetCurrentThread(), mask);
}
#endif

static bool get_blocktemplate(CURL *curl, struct work *work);

void get_currentalgo(char* buf, int sz)
{
	snprintf(buf, sz, "%s", algo_names[opt_algo]);
}

/**
 * Exit app
 */
void proper_exit(int reason)
{
	abort_flag = true;
	usleep(200 * 1000);
	cuda_shutdown();

	if (reason == EXIT_CODE_OK && app_exit_code != EXIT_CODE_OK) {
		reason = app_exit_code;
	}

	if (check_dups)
		hashlog_purge_all();
	stats_purge_all();

#ifdef WIN32
	timeEndPeriod(1); // else never executed
#endif
#ifdef USE_WRAPNVML
	if (hnvml)
		nvml_destroy(hnvml);
#endif
	free(opt_syslog_pfx);
	free(opt_api_allow);
	free(work_restart);
	//free(thr_info);
	exit(reason);
}

static bool jobj_binary(const json_t *obj, const char *key,
			void *buf, size_t buflen)
{
	const char *hexstr;
	json_t *tmp;

	tmp = json_object_get(obj, key);
	if (unlikely(!tmp)) {
		applog(LOG_ERR, "JSON key '%s' not found", key);
		return false;
	}
	hexstr = json_string_value(tmp);
	if (unlikely(!hexstr)) {
		applog(LOG_ERR, "JSON key '%s' is not a string", key);
		return false;
	}
	if (!hex2bin((uchar*)buf, hexstr, buflen))
		return false;

	return true;
}

/* compute nbits to get the network diff */
static void calc_network_diff(struct work *work)
{
	// sample for diff 43.281 : 1c05ea29
	uchar rtarget[48] = { 0 };
	uint64_t diffone = 0xFFFF000000000000ull; //swab64(0xFFFFull);
	uint64_t *data64, d64;
	// todo: endian reversed on longpoll could be zr5 specific...
	uint32_t nbits = have_longpoll ? work->data[18] : swab32(work->data[18]);
	uint32_t shift = (swab32(nbits) & 0xff); // 0x1c = 28
	uint32_t bits = (nbits & 0xffffff);
	int shfb = 8 * (26 - (shift - 3));

	switch (opt_algo) {
		case ALGO_ANIME:
		case ALGO_QUARK:
			diffone = 0xFFFFFF0000000000ull;
			break;
		case ALGO_PLUCK:
		case ALGO_SCRYPT:
		case ALGO_SCRYPT_JANE:
			// cant get the right value on these 3 algos...
			diffone = 0xFFFFFFFF00000000ull;
			net_diff = 0.;
			break;
		case ALGO_NEOSCRYPT:
			// todo/check... (neoscrypt data is reversed)
			if (opt_debug)
				applog(LOG_DEBUG, "diff: %08x -> shift %u, bits %08x, shfb %d", nbits, shift, bits, shfb);
			net_diff = 0.;
			return;
	}

	if (shift >= 3 && shift < sizeof(rtarget)-3) {
		memcpy(&rtarget[shift - 3], &bits, 3); // 0029ea05 00000000
	}
	swab256(rtarget, rtarget);

	data64 = (uint64_t*)(rtarget + 4);

	switch (opt_algo) {
		case ALGO_HEAVY:
			data64 = (uint64_t*)(rtarget + 2);
			break;
		case ALGO_ANIME:
		case ALGO_QUARK:
			data64 = (uint64_t*)(rtarget + 3);
			break;
	}

	d64 = swab64(*data64);
	if (!d64)
		d64 = 1;
	net_diff = (double)diffone / d64; // 43.281
	if (opt_debug)
		applog(LOG_DEBUG, "diff: %08x -> shift %u, bits %08x, shfb %d -> %.5f (pool %u)",
			nbits, shift, bits, shfb, net_diff, work->pooln);
}

static bool work_decode(const json_t *val, struct work *work)
{
	int data_size = sizeof(work->data), target_size = sizeof(work->target);
	int adata_sz = ARRAY_SIZE(work->data), atarget_sz = ARRAY_SIZE(work->target);
	int i;

	if (opt_algo == ALGO_NEOSCRYPT || opt_algo == ALGO_ZR5) {
		data_size = 80; adata_sz = 20;
	}

	if (unlikely(!jobj_binary(val, "data", work->data, data_size))) {
		applog(LOG_ERR, "JSON inval data");
		return false;
	}
	if (unlikely(!jobj_binary(val, "target", work->target, target_size))) {
		applog(LOG_ERR, "JSON inval target");
		return false;
	}

	if (opt_algo == ALGO_HEAVY) {
		if (unlikely(!jobj_binary(val, "maxvote", &work->maxvote, sizeof(work->maxvote)))) {
			work->maxvote = 2048;
		}
	} else work->maxvote = 0;

	for (i = 0; i < adata_sz; i++)
		work->data[i] = le32dec(work->data + i);
	for (i = 0; i < atarget_sz; i++)
		work->target[i] = le32dec(work->target + i);

	if (opt_max_diff > 0. && !allow_mininginfo)
		calc_network_diff(work);

	json_t *jr = json_object_get(val, "noncerange");
	if (jr) {
		const char * hexstr = json_string_value(jr);
		if (likely(hexstr)) {
			// never seen yet...
			hex2bin((uchar*)work->noncerange.u64, hexstr, 8);
			applog(LOG_DEBUG, "received noncerange: %08x-%08x",
				work->noncerange.u32[0], work->noncerange.u32[1]);
		}
	}

	/* use work ntime as job id (solo-mining) */
	cbin2hex(work->job_id, (const char*)&work->data[17], 4);

	return true;
}

/**
 * Calculate the work difficulty as double
 */
static void calc_target_diff(struct work *work)
{
	// sample for diff 32.53 : 00000007de5f0000
	char rtarget[32];
	uint64_t diffone = 0xFFFF000000000000ull;
	uint64_t *data64, d64;

	swab256(rtarget, work->target);

	data64 = (uint64_t*)(rtarget + 3);

	switch (opt_algo) {
		case ALGO_NEOSCRYPT: /* diffone in work->target[7] ? */
		//case ALGO_SCRYPT:
		//case ALGO_SCRYPT_JANE:
			// todo/check...
			work->difficulty = 0.;
			return;
		case ALGO_HEAVY:
			data64 = (uint64_t*)(rtarget + 2);
			break;
	}

	d64 = swab64(*data64);
	if (unlikely(!d64))
		d64 = 1;
	work->difficulty = (double)diffone / d64;
	if (opt_difficulty > 0.) {
		work->difficulty /= opt_difficulty;
	}
}

static int share_result(int result, const char *reason)
{
	char s[32] = { 0 };
	double hashrate = 0.;
	struct pool_infos *p = &pools[cur_pooln];

	pthread_mutex_lock(&stats_lock);

	for (int i = 0; i < opt_n_threads; i++) {
		hashrate += stats_get_speed(i, thr_hashrates[i]);
	}

	result ? p->accepted_count++ : p->rejected_count++;
	pthread_mutex_unlock(&stats_lock);

	global_hashrate = llround(hashrate);

	format_hashrate(hashrate, s);
	applog(LOG_NOTICE, "accepted: %lu/%lu (%.2f%%), %s %s",
			p->accepted_count,
			p->accepted_count + p->rejected_count,
			100. * p->accepted_count / (p->accepted_count + p->rejected_count),
			s,
			use_colors ?
				(result ? CL_GRN "yay!!!" : CL_RED "booooo")
			:	(result ? "(yay!!!)" : "(booooo)"));

	if (reason) {
		applog(LOG_WARNING, "reject reason: %s", reason);
		if (strncasecmp(reason, "low difficulty", 14) == 0) {
			opt_difficulty = (opt_difficulty * 2.0) / 3.0;
			applog(LOG_WARNING, "factor reduced to : %0.2f", opt_difficulty);
			return 0;
		}
		if (!check_dups && strncasecmp(reason, "duplicate", 9) == 0) {
			applog(LOG_WARNING, "enabling duplicates check feature");
			check_dups = true;
		}
	}
	return 1;
}

static bool submit_upstream_work(CURL *curl, struct work *work)
{
	json_t *val, *res, *reason;
	bool stale_work = false;
	char s[384];

	/* discard work sent by other pools */
	if (work->pooln != cur_pooln) {
		return true;
	}

	/* discard if a newer bloc was received */
	stale_work = work->height && work->height < g_work.height;
	if (have_stratum && !stale_work && opt_algo != ALGO_ZR5 && opt_algo != ALGO_SCRYPT_JANE) {
		pthread_mutex_lock(&g_work_lock);
		if (strlen(work->job_id + 8))
			stale_work = strncmp(work->job_id + 8, g_work.job_id + 8, 4);
		pthread_mutex_unlock(&g_work_lock);
	}

	if (!have_stratum && !stale_work && allow_gbt) {
		struct work wheight = { 0 };
		if (get_blocktemplate(curl, &wheight)) {
			if (work->height && work->height < wheight.height) {
				if (opt_debug)
					applog(LOG_WARNING, "bloc %u was already solved", work->height);
				return true;
			}
		}
	}

	if (!stale_work && opt_algo == ALGO_ZR5 && !have_stratum) {
		stale_work = (memcmp(&work->data[1], &g_work.data[1], 68));
	}

	if (!submit_old && stale_work) {
		if (opt_debug)
			applog(LOG_WARNING, "stale work detected, discarding");
		return true;
	}
	calc_target_diff(work);

	if (have_stratum) {
		uint32_t sent = 0;
		uint32_t ntime, nonce;
		uint16_t nvote;
		char *ntimestr, *noncestr, *xnonce2str, *nvotestr;

		switch (opt_algo) {
		case ALGO_ZR5:
			check_dups = true;
			be32enc(&ntime, work->data[17]);
			be32enc(&nonce, work->data[19]);
			break;
		default:
			le32enc(&ntime, work->data[17]);
			le32enc(&nonce, work->data[19]);
		}
		noncestr = bin2hex((const uchar*)(&nonce), 4);

		if (check_dups)
			sent = hashlog_already_submittted(work->job_id, nonce);
		if (sent > 0) {
			sent = (uint32_t) time(NULL) - sent;
			if (!opt_quiet) {
				applog(LOG_WARNING, "nonce %s was already sent %u seconds ago", noncestr, sent);
				hashlog_dump_job(work->job_id);
			}
			free(noncestr);
			// prevent useless computing on some pools
			g_work_time = 0;
			restart_threads();
			return true;
		}

		ntimestr = bin2hex((const uchar*)(&ntime), 4);
		xnonce2str = bin2hex(work->xnonce2, work->xnonce2_len);

		if (opt_algo == ALGO_HEAVY) {
			be16enc(&nvote, *((uint16_t*)&work->data[20]));
			nvotestr = bin2hex((const uchar*)(&nvote), 2);
			sprintf(s,
				"{\"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\"], \"id\":4}",
				rpc_user, work->job_id + 8, xnonce2str, ntimestr, noncestr, nvotestr);
			free(nvotestr);
		} else {
			sprintf(s,
				"{\"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%s\", \"%s\"], \"id\":4}",
				rpc_user, work->job_id + 8, xnonce2str, ntimestr, noncestr);
		}
		free(xnonce2str);
		free(ntimestr);
		free(noncestr);

		gettimeofday(&stratum.tv_submit, NULL);
		if (unlikely(!stratum_send_line(&stratum, s))) {
			applog(LOG_ERR, "submit_upstream_work stratum_send_line failed");
			return false;
		}

		if (check_dups)
			hashlog_remember_submit(work, nonce);

	} else {

		int data_size = sizeof(work->data);
		int adata_sz = ARRAY_SIZE(work->data);

		/* build hex string */
		char *str = NULL;

		if (opt_algo == ALGO_ZR5) {
			data_size = 80; adata_sz = 20;
		}

		if (opt_algo != ALGO_HEAVY && opt_algo != ALGO_MJOLLNIR) {
			for (int i = 0; i < adata_sz; i++)
				le32enc(work->data + i, work->data[i]);
		}
		str = bin2hex((uchar*)work->data, data_size);
		if (unlikely(!str)) {
			applog(LOG_ERR, "submit_upstream_work OOM");
			return false;
		}

		/* build JSON-RPC request */
		sprintf(s,
			"{\"method\": \"getwork\", \"params\": [\"%s\"], \"id\":4}\r\n",
			str);

		/* issue JSON-RPC request */
		val = json_rpc_call(curl, rpc_url, rpc_userpass, s, false, false, NULL);
		if (unlikely(!val)) {
			applog(LOG_ERR, "submit_upstream_work json_rpc_call failed");
			return false;
		}

		res = json_object_get(val, "result");
		reason = json_object_get(val, "reject-reason");
		if (!share_result(json_is_true(res), reason ? json_string_value(reason) : NULL)) {
			if (check_dups)
				hashlog_purge_job(work->job_id);
		}

		json_decref(val);

		free(str);
	}

	return true;
}

/* simplified method to only get some extra infos in solo mode */
static bool gbt_work_decode(const json_t *val, struct work *work)
{
	json_t *err = json_object_get(val, "error");
	if (err && !json_is_null(err)) {
		allow_gbt = false;
		applog(LOG_INFO, "GBT not supported, bloc height unavailable");
		return false;
	}

	if (!work->height) {
		// complete missing data from getwork
		json_t *key = json_object_get(val, "height");
		if (key && json_is_integer(key)) {
			work->height = (uint32_t) json_integer_value(key);
			if (!opt_quiet && work->height > g_work.height) {
				if (net_diff > 0.) {
					char netinfo[64] = { 0 };
					char srate[32] = { 0 };
					sprintf(netinfo, "diff %.2f", net_diff);
					if (net_hashrate) {
						format_hashrate((double) net_hashrate, srate);
						strcat(netinfo, ", net ");
						strcat(netinfo, srate);
					}
					applog(LOG_BLUE, "%s block %d, %s",
						algo_names[opt_algo], work->height, netinfo);
				} else {
					applog(LOG_BLUE, "%s %s block %d", short_url,
						algo_names[opt_algo], work->height);
				}
				g_work.height = work->height;
			}
		}
	}

	return true;
}

#define GBT_CAPABILITIES "[\"coinbasetxn\", \"coinbasevalue\", \"longpoll\", \"workid\"]"
static const char *gbt_req =
	"{\"method\": \"getblocktemplate\", \"params\": ["
	//	"{\"capabilities\": " GBT_CAPABILITIES "}"
	"], \"id\":9}\r\n";

static bool get_blocktemplate(CURL *curl, struct work *work)
{
	if (!allow_gbt)
		return false;

	int curl_err = 0;
	json_t *val = json_rpc_call(curl, rpc_url, rpc_userpass, gbt_req,
			    false, false, &curl_err);

	if (!val && curl_err == -1) {
		// when getblocktemplate is not supported, disable it
		allow_gbt = false;
		if (!opt_quiet) {
				applog(LOG_BLUE, "gbt not supported, block height notices disabled");
		}
		return false;
	}

	bool rc = gbt_work_decode(json_object_get(val, "result"), work);

	json_decref(val);

	return rc;
}

// good alternative for wallet mining, difficulty and net hashrate
static const char *info_req =
	"{\"method\": \"getmininginfo\", \"params\": [], \"id\":8}\r\n";

static bool get_mininginfo(CURL *curl, struct work *work)
{
	if (have_stratum || !allow_mininginfo)
		return false;

	int curl_err = 0;
	json_t *val = json_rpc_call(curl, rpc_url, rpc_userpass, info_req,
			    want_longpoll, have_longpoll, &curl_err);

	if (!val && curl_err == -1) {
		allow_mininginfo = false;
		if (opt_debug) {
				applog(LOG_DEBUG, "getmininginfo not supported");
		}
		return false;
	} else {
		json_t *res = json_object_get(val, "result");
		// "blocks": 491493 (= current work height - 1)
		// "difficulty": 0.99607860999999998
		// "networkhashps": 56475980
		if (res) {
			json_t *key = json_object_get(res, "difficulty");
			if (key && json_is_real(key)) {
				net_diff = json_real_value(key);
			}
			key = json_object_get(res, "networkhashps");
			if (key && json_is_integer(key)) {
				net_hashrate = json_integer_value(key);
			}
			key = json_object_get(res, "blocks");
			if (key && json_is_integer(key)) {
				net_blocks = json_integer_value(key);
			}
		}
	}
	json_decref(val);
	return true;
}

static const char *rpc_req =
	"{\"method\": \"getwork\", \"params\": [], \"id\":0}\r\n";

static bool get_upstream_work(CURL *curl, struct work *work)
{
	json_t *val;
	bool rc;
	struct timeval tv_start, tv_end, diff;

	/* assign pool number before rpc calls */
	work->pooln = cur_pooln;

	gettimeofday(&tv_start, NULL);
	val = json_rpc_call(curl, rpc_url, rpc_userpass, rpc_req,
			    want_longpoll, false, NULL);
	gettimeofday(&tv_end, NULL);

	if (have_stratum) {
		if (val)
			json_decref(val);
		return true;
	}

	if (!val)
		return false;

	rc = work_decode(json_object_get(val, "result"), work);

	if (opt_protocol && rc) {
		timeval_subtract(&diff, &tv_end, &tv_start);
		/* show time because curl can be slower against versions/config */
		applog(LOG_DEBUG, "got new work in %.2f ms",
		       (1000.0 * diff.tv_sec) + (0.001 * diff.tv_usec));
	}

	json_decref(val);

	get_mininginfo(curl, work);
	get_blocktemplate(curl, work);

	return rc;
}

static void workio_cmd_free(struct workio_cmd *wc)
{
	if (!wc)
		return;

	switch (wc->cmd) {
	case WC_SUBMIT_WORK:
		aligned_free(wc->u.work);
		break;
	default: /* do nothing */
		break;
	}

	memset(wc, 0, sizeof(*wc));	/* poison */
	free(wc);
}

static void workio_abort()
{
	struct workio_cmd *wc;

	/* fill out work request message */
	wc = (struct workio_cmd *)calloc(1, sizeof(*wc));
	if (!wc)
		return;

	wc->cmd = WC_ABORT;

	/* send work request to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc)) {
		workio_cmd_free(wc);
	}
}

static bool workio_get_work(struct workio_cmd *wc, CURL *curl)
{
	struct work *ret_work;
	int failures = 0;

	ret_work = (struct work*)aligned_calloc(sizeof(*ret_work));
	if (!ret_work)
		return false;

	/* obtain new work from bitcoin via JSON-RPC */
	while (!get_upstream_work(curl, ret_work)) {
		if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
			applog(LOG_ERR, "json_rpc_call failed, terminating workio thread");
			aligned_free(ret_work);
			return false;
		}

		/* pause, then restart work-request loop */
		applog(LOG_ERR, "json_rpc_call failed, retry after %d seconds",
			opt_fail_pause);
		sleep(opt_fail_pause);
	}

	/* send work to requesting thread */
	if (!tq_push(wc->thr->q, ret_work))
		aligned_free(ret_work);

	return true;
}

static bool workio_submit_work(struct workio_cmd *wc, CURL *curl)
{
	int failures = 0;

	/* submit solution to bitcoin via JSON-RPC */
	while (!submit_upstream_work(curl, wc->u.work)) {
		uint32_t pooln = wc->u.work->pooln;
		if (pooln != cur_pooln) {
			applog(LOG_DEBUG, "discarding work from pool %u", pooln);
			return true;
		}
		if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
			applog(LOG_ERR, "...terminating workio thread");
			return false;
		}

		/* pause, then restart work-request loop */
		if (!opt_benchmark)
			applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);

		sleep(opt_fail_pause);
	}

	return true;
}

static void *workio_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info*)userdata;
	CURL *curl;
	bool ok = true;

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		applog(LOG_ERR, "CURL initialization failed");
		return NULL;
	}

	while (ok) {
		struct workio_cmd *wc;

		/* wait for workio_cmd sent to us, on our queue */
		wc = (struct workio_cmd *)tq_pop(mythr->q, NULL);
		if (!wc) {
			ok = false;
			break;
		}

		/* process workio_cmd */
		switch (wc->cmd) {
		case WC_GET_WORK:
			ok = workio_get_work(wc, curl);
			break;
		case WC_SUBMIT_WORK:
			ok = workio_submit_work(wc, curl);
			break;
		case WC_ABORT:
		default:		/* should never happen */
			ok = false;
			break;
		}

		workio_cmd_free(wc);
	}

	if (opt_debug_threads)
		applog(LOG_DEBUG, "%s() died", __func__);
	curl_easy_cleanup(curl);
	tq_freeze(mythr->q);
	return NULL;
}

static bool get_work(struct thr_info *thr, struct work *work)
{
	struct workio_cmd *wc;
	struct work *work_heap;

	if (opt_benchmark) {
		memset(work->data, 0x55, 76);
		//work->data[17] = swab32((uint32_t)time(NULL));
		memset(work->data + 19, 0x00, 52);
		work->data[20] = 0x80000000;
		work->data[31] = 0x00000280;
		memset(work->target, 0x00, sizeof(work->target));
		return true;
	}

	/* fill out work request message */
	wc = (struct workio_cmd *)calloc(1, sizeof(*wc));
	if (!wc)
		return false;

	wc->cmd = WC_GET_WORK;
	wc->thr = thr;

	/* send work request to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc)) {
		workio_cmd_free(wc);
		return false;
	}

	/* wait for response, a unit of work */
	work_heap = (struct work *)tq_pop(thr->q, NULL);
	if (!work_heap)
		return false;

	/* copy returned work into storage provided by caller */
	memcpy(work, work_heap, sizeof(*work));
	aligned_free(work_heap);

	return true;
}

static bool submit_work(struct thr_info *thr, const struct work *work_in)
{
	struct workio_cmd *wc;
	/* fill out work request message */
	wc = (struct workio_cmd *)calloc(1, sizeof(*wc));
	if (!wc)
		return false;

	wc->u.work = (struct work *)aligned_calloc(sizeof(*work_in));
	if (!wc->u.work)
		goto err_out;

	wc->cmd = WC_SUBMIT_WORK;
	wc->thr = thr;
	memcpy(wc->u.work, work_in, sizeof(*work_in));

	/* send solution to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc))
		goto err_out;

	return true;

err_out:
	workio_cmd_free(wc);
	return false;
}

static void stratum_gen_work(struct stratum_ctx *sctx, struct work *work)
{
	uchar merkle_root[64];
	int i;

	if (!sctx->job.job_id) {
		// applog(LOG_WARNING, "stratum_gen_work: job not yet retrieved");
		return;
	}

	pthread_mutex_lock(&sctx->work_lock);

	work->pooln = cur_pooln;

	// store the job ntime as high part of jobid
	snprintf(work->job_id, sizeof(work->job_id), "%07x %s",
		be32dec(sctx->job.ntime) & 0xfffffff, sctx->job.job_id);
	work->xnonce2_len = sctx->xnonce2_size;
	memcpy(work->xnonce2, sctx->job.xnonce2, sctx->xnonce2_size);

	// also store the bloc number
	work->height = sctx->job.height;

	/* Generate merkle root */
	switch (opt_algo) {
		case ALGO_HEAVY:
		case ALGO_MJOLLNIR:
			heavycoin_hash(merkle_root, sctx->job.coinbase, (int)sctx->job.coinbase_size);
			break;
		case ALGO_FUGUE256:
		case ALGO_GROESTL:
		case ALGO_KECCAK:
		case ALGO_BLAKECOIN:
			SHA256((uchar*)sctx->job.coinbase, sctx->job.coinbase_size, (uchar*)merkle_root);
			break;
		default:
			sha256d(merkle_root, sctx->job.coinbase, (int)sctx->job.coinbase_size);
	}

	for (i = 0; i < sctx->job.merkle_count; i++) {
		memcpy(merkle_root + 32, sctx->job.merkle[i], 32);
		if (opt_algo == ALGO_HEAVY || opt_algo == ALGO_MJOLLNIR)
			heavycoin_hash(merkle_root, merkle_root, 64);
		else
			sha256d(merkle_root, merkle_root, 64);
	}
	
	/* Increment extranonce2 */
	for (i = 0; i < (int)sctx->xnonce2_size && !++sctx->job.xnonce2[i]; i++);

	/* Assemble block header */
	memset(work->data, 0, sizeof(work->data));
	work->data[0] = le32dec(sctx->job.version);
	for (i = 0; i < 8; i++)
		work->data[1 + i] = le32dec((uint32_t *)sctx->job.prevhash + i);
	for (i = 0; i < 8; i++)
		work->data[9 + i] = be32dec((uint32_t *)merkle_root + i);
	work->data[17] = le32dec(sctx->job.ntime);
	work->data[18] = le32dec(sctx->job.nbits);

	if (opt_max_diff > 0.)
		calc_network_diff(work);

	switch (opt_algo) {
	case ALGO_MJOLLNIR:
	case ALGO_HEAVY:
		// todo: check if 19 is enough
		for (i = 0; i < 20; i++)
			work->data[i] = be32dec((uint32_t *)&work->data[i]);
		break;
	case ALGO_ZR5:
		for (i = 0; i < 19; i++)
			work->data[i] = be32dec((uint32_t *)&work->data[i]);
		break;
	}

	work->data[20] = 0x80000000;
	work->data[31] = (opt_algo == ALGO_MJOLLNIR) ? 0x000002A0 : 0x00000280;

	// HeavyCoin (vote / reward)
	if (opt_algo == ALGO_HEAVY) {
		work->maxvote = 2048;
		uint16_t *ext = (uint16_t*)(&work->data[20]);
		ext[0] = opt_vote;
		ext[1] = be16dec(sctx->job.nreward);
		// applog(LOG_DEBUG, "DEBUG: vote=%hx reward=%hx", ext[0], ext[1]);
	}

	pthread_mutex_unlock(&sctx->work_lock);

	if (opt_debug) {
		uint32_t utm = work->data[17];
		if (opt_algo != ALGO_ZR5) utm = swab32(utm);
		char *tm = atime2str(utm - sctx->srvtime_diff);
		char *xnonce2str = bin2hex(work->xnonce2, sctx->xnonce2_size);
		applog(LOG_DEBUG, "DEBUG: job_id=%s xnonce2=%s time=%s",
		       work->job_id, xnonce2str, tm);
		free(tm);
		free(xnonce2str);
	}

	switch (opt_algo) {
		case ALGO_JACKPOT:
		case ALGO_NEOSCRYPT:
		case ALGO_PLUCK:
		case ALGO_SCRYPT:
		case ALGO_SCRYPT_JANE:
			diff_to_target(work->target, sctx->job.diff / (65536.0 * opt_difficulty));
			break;
		case ALGO_DMD_GR:
		case ALGO_FRESH:
		case ALGO_FUGUE256:
		case ALGO_GROESTL:
			diff_to_target(work->target, sctx->job.diff / (256.0 * opt_difficulty));
			break;
		case ALGO_KECCAK:
		case ALGO_LYRA2:
			diff_to_target(work->target, sctx->job.diff / (128.0 * opt_difficulty));
			break;
		default:
			diff_to_target(work->target, sctx->job.diff / opt_difficulty);
	}
}

void restart_threads(void)
{
	if (opt_debug && !opt_quiet)
		applog(LOG_DEBUG,"%s", __FUNCTION__);

	for (int i = 0; i < opt_n_threads; i++)
		work_restart[i].restart = 1;
}

static bool wanna_mine(int thr_id)
{
	bool state = true;
	bool allow_pool_rotate = (thr_id == 0 && num_pools > 1 && !pool_is_switching);

	if (opt_max_temp > 0.0) {
#ifdef USE_WRAPNVML
		struct cgpu_info * cgpu = &thr_info[thr_id].gpu;
		float temp = gpu_temp(cgpu);
		if (temp > opt_max_temp) {
			if (!conditional_state[thr_id] && !opt_quiet)
				applog(LOG_INFO, "GPU #%d: temperature too high (%.0f°c), waiting...",
					device_map[thr_id], temp);
			state = false;
		}
#endif
	}
	if (opt_max_diff > 0.0 && net_diff > opt_max_diff) {
		int next = pool_get_first_valid(cur_pooln+1);
		if (num_pools > 1 && pools[next].max_diff != pools[cur_pooln].max_diff)
			conditional_pool_rotate = allow_pool_rotate;
		if (!conditional_state[thr_id] && !opt_quiet && !thr_id)
			applog(LOG_INFO, "network diff too high, waiting...");
		state = false;
	}
	if (opt_max_rate > 0.0 && net_hashrate > opt_max_rate) {
		int next = pool_get_first_valid(cur_pooln+1);
		if (pools[next].max_rate != pools[cur_pooln].max_rate)
			conditional_pool_rotate = allow_pool_rotate;
		if (!conditional_state[thr_id] && !opt_quiet && !thr_id) {
			char rate[32];
			format_hashrate(opt_max_rate, rate);
			applog(LOG_INFO, "network hashrate too high, waiting %s...", rate);
		}
		state = false;
	}
	conditional_state[thr_id] = (uint8_t) !state;
	return state;
}

static void *miner_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info *)userdata;
	int thr_id = mythr->id;
	struct work work;
	uint32_t max_nonce;
	uint32_t end_nonce = UINT32_MAX / opt_n_threads * (thr_id + 1) - (thr_id + 1);
	bool work_done = false;
	bool extrajob = false;
	char s[16];
	int rc = 0;

	memset(&work, 0, sizeof(work)); // prevent work from being used uninitialized

	if (opt_priority > 0) {
		int prio = 2; // default to normal
#ifndef WIN32
		prio = 0;
		// note: different behavior on linux (-19 to 19)
		switch (opt_priority) {
			case 0:
				prio = 15;
				break;
			case 1:
				prio = 5;
				break;
			case 2:
				prio = 0; // normal process
				break;
			case 3:
				prio = -1; // above
				break;
			case 4:
				prio = -10;
				break;
			case 5:
				prio = -15;
		}
		if (opt_debug)
			applog(LOG_DEBUG, "Thread %d priority %d (nice %d)",
				thr_id,	opt_priority, prio);
#endif
		setpriority(PRIO_PROCESS, 0, prio);
		drop_policy();
	}

	/* Cpu thread affinity */
	if (num_cpus > 1) {
		if (opt_affinity == -1 && opt_n_threads > 1) {
			if (opt_debug)
				applog(LOG_DEBUG, "Binding thread %d to cpu %d (mask %x)", thr_id,
						thr_id % num_cpus, (1 << (thr_id % num_cpus)));
			affine_to_cpu_mask(thr_id, 1 << (thr_id % num_cpus));
		} else if (opt_affinity != -1) {
			if (opt_debug)
				applog(LOG_DEBUG, "Binding thread %d to cpu mask %x", thr_id,
						opt_affinity);
			affine_to_cpu_mask(thr_id, opt_affinity);
		}
	}

	while (1) {
		struct timeval tv_start, tv_end, diff;
		unsigned long hashes_done;
		uint32_t start_nonce;
		uint32_t scan_time = have_longpoll ? LP_SCANTIME : opt_scantime;
		uint64_t max64, minmax = 0x100000;

		// &work.data[19]
		int wcmplen = 76;
		int wcmpoft = 0;
		uint32_t *nonceptr = (uint32_t*) (((char*)work.data) + wcmplen);

		if (have_stratum) {
			uint32_t sleeptime = 0;
			while (!work_done && time(NULL) >= (g_work_time + opt_scantime)) {
				usleep(100*1000);
				if (sleeptime > 4) {
					extrajob = true;
					break;
				}
				sleeptime++;
			}
			if (sleeptime && opt_debug && !opt_quiet)
				applog(LOG_DEBUG, "sleeptime: %u ms", sleeptime*100);
			nonceptr = (uint32_t*) (((char*)work.data) + wcmplen);
			pthread_mutex_lock(&g_work_lock);
			extrajob |= work_done;
			if (nonceptr[0] >= end_nonce || extrajob) {
				work_done = false;
				extrajob = false;
				stratum_gen_work(&stratum, &g_work);
			}
		} else {
			pthread_mutex_lock(&g_work_lock);
			if ((time(NULL) - g_work_time) >= scan_time || nonceptr[0] >= (end_nonce - 0x100)) {
				if (opt_debug && g_work_time && !opt_quiet)
					applog(LOG_DEBUG, "work time %u/%us nonce %x/%x", (time(NULL) - g_work_time),
						scan_time, nonceptr[0], end_nonce);
				/* obtain new work from internal workio thread */
				if (unlikely(!get_work(mythr, &g_work))) {
					pthread_mutex_unlock(&g_work_lock);
					applog(LOG_ERR, "work retrieval failed, exiting mining thread %d", mythr->id);
					goto out;
				}
				g_work_time = time(NULL);
			}
		}

		if (!opt_benchmark && (g_work.height != work.height || memcmp(work.target, g_work.target, sizeof(work.target))))
		{
			calc_target_diff(&g_work);
			if (opt_debug) {
				uint64_t target64 = g_work.target[7] * 0x100000000ULL + g_work.target[6];
				applog(LOG_DEBUG, "job %s target change: %llx (%.1f)", g_work.job_id, target64, g_work.difficulty);
			}
			memcpy(work.target, g_work.target, sizeof(work.target));
			work.difficulty = g_work.difficulty;
			work.height = g_work.height;
			nonceptr[0] = (UINT32_MAX / opt_n_threads) * thr_id; // 0 if single thr
			/* on new target, ignoring nonce, clear sent data (hashlog) */
			if (memcmp(work.target, g_work.target, sizeof(work.target))) {
				if (check_dups)
					hashlog_purge_job(work.job_id);
			}
		}

		if (opt_algo == ALGO_ZR5) {
			// ignore pok/version header
			wcmpoft = 1;
			wcmplen -= 4;
		}

		if (memcmp(&work.data[wcmpoft], &g_work.data[wcmpoft], wcmplen)) {
			#if 0
			if (opt_debug) {
				for (int n=0; n <= (wcmplen-8); n+=8) {
					if (memcmp(work.data + n, g_work.data + n, 8)) {
						applog(LOG_DEBUG, "job %s work updated at offset %d:", g_work.job_id, n);
						applog_hash((uchar*) &work.data[n]);
						applog_compare_hash((uchar*) &g_work.data[n], (uchar*) &work.data[n]);
					}
				}
			}
			#endif
			memcpy(&work, &g_work, sizeof(struct work));
			nonceptr[0] = (UINT32_MAX / opt_n_threads) * thr_id; // 0 if single thr
		} else
			nonceptr[0]++; //??

		work_restart[thr_id].restart = 0;
		pthread_mutex_unlock(&g_work_lock);

		/* conditional mining */
		if (!wanna_mine(thr_id)) {

			// conditional pool switch
			if (num_pools > 1 && conditional_pool_rotate) {
				if (!pool_is_switching)
					pool_switch_next();
				else if (time(NULL) - firstwork_time > 35) {
					if (!opt_quiet)
						applog(LOG_WARNING, "Pool switching timed out...");
					pools[cur_pooln].wait_time += 1;
					pool_is_switching = false;
				}
				sleep(1);
				continue;
			}

			sleep(5); pools[cur_pooln].wait_time += 5;
			continue;
		}

		/* prevent gpu scans before a job is received */
		if (have_stratum && !firstwork_time && work.data[0] == 0) {
			sleep(1);
			continue;
		}

		/* adjust max_nonce to meet target scan time */
		if (have_stratum)
			max64 = LP_SCANTIME;
		else
			max64 = max(1, scan_time + g_work_time - time(NULL));

		/* time limit */
		if (opt_time_limit && firstwork_time) {
			int passed = (int)(time(NULL) - firstwork_time);
			int remain = (int)(opt_time_limit - passed);
			if (remain < 0)  {
				if (num_pools > 1 && pools[cur_pooln].time_limit) {
					if (!pool_is_switching) {
						if (!opt_quiet)
							applog(LOG_NOTICE, "Pool timeout of %ds reached, rotate...", opt_time_limit);
						pool_switch_next();
					} else {
						// ensure we dont stay locked there...
						if (time(NULL) - firstwork_time > 35) {
							applog(LOG_WARNING, "Pool switching timed out...");
							pools[cur_pooln].wait_time += 1;
							pool_is_switching = false;
						}
					}
					sleep(1);
					continue;
				}
				app_exit_code = EXIT_CODE_TIME_LIMIT;
				abort_flag = true;
				if (opt_benchmark) {
					char rate[32];
					format_hashrate((double)global_hashrate, rate);
	                                applog(LOG_NOTICE, "Benchmark: %s", rate);
					usleep(200*1000);
					fprintf(stderr, "%llu\n", (long long unsigned int) global_hashrate);
				} else {
					applog(LOG_NOTICE,
						"Mining timeout of %ds reached, exiting...", opt_time_limit);
				}
				workio_abort();
				break;
			}
			if (remain < max64) max64 = remain;
		}

		max64 *= (uint32_t)thr_hashrates[thr_id];

		/* on start, max64 should not be 0,
		 *    before hashrate is computed */
		if (max64 < minmax) {
			switch (opt_algo) {
			case ALGO_BLAKECOIN:
			case ALGO_BLAKE:
				minmax = 0x80000000U;
				break;
			case ALGO_KECCAK:
				minmax = 0x40000000U;
				break;
			case ALGO_DOOM:
			case ALGO_JACKPOT:
			case ALGO_LUFFA_DOOM:
				minmax = 0x2000000;
				break;
			case ALGO_S3:
			case ALGO_X11:
			case ALGO_X13:
				minmax = 0x400000;
				break;
			case ALGO_LYRA2:
			case ALGO_NEOSCRYPT:
			case ALGO_SCRYPT:
			case ALGO_SCRYPT_JANE:
				minmax = 0x100000;
				break;
			case ALGO_PLUCK:
				minmax = 0x2000;
				break;
			}
			max64 = max(minmax-1, max64);
		}

		// we can't scan more than uint32 capacity
		max64 = min(UINT32_MAX, max64);

		start_nonce = nonceptr[0];

		/* never let small ranges at end */
		if (end_nonce >= UINT32_MAX - 256)
			end_nonce = UINT32_MAX;

		if ((max64 + start_nonce) >= end_nonce)
			max_nonce = end_nonce;
		else
			max_nonce = (uint32_t) (max64 + start_nonce);

		// todo: keep it rounded for gpu threads ?

		if (unlikely(start_nonce > max_nonce)) {
			// should not happen but seen in skein2 benchmark with 2 gpus
			max_nonce = end_nonce = UINT32_MAX;
		}

		work.scanned_from = start_nonce;
		nonceptr[0] = start_nonce;

		if (opt_debug)
			applog(LOG_DEBUG, "GPU #%d: start=%08x end=%08x range=%08x",
				device_map[thr_id], start_nonce, max_nonce, (max_nonce-start_nonce));

		hashes_done = 0;
		gettimeofday(&tv_start, NULL);

		/* scan nonces for a proof-of-work hash */
		switch (opt_algo) {

		case ALGO_HEAVY:
			rc = scanhash_heavy(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done, work.maxvote, HEAVYCOIN_BLKHDR_SZ);
			break;

		case ALGO_KECCAK:
			rc = scanhash_keccak256(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_MJOLLNIR:
			rc = scanhash_heavy(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done, 0, MNR_BLKHDR_SZ);
			break;

		case ALGO_DEEP:
			rc = scanhash_deep(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_DOOM:
		case ALGO_LUFFA_DOOM:
			rc = scanhash_doom(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_FUGUE256:
			rc = scanhash_fugue256(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_GROESTL:
		case ALGO_DMD_GR:
			rc = scanhash_groestlcoin(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_MYR_GR:
			rc = scanhash_myriad(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_JACKPOT:
			rc = scanhash_jackpot(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_QUARK:
			rc = scanhash_quark(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_QUBIT:
			rc = scanhash_qubit(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_ANIME:
			rc = scanhash_anime(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_BLAKECOIN:
			rc = scanhash_blake256(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done, 8);
			break;

		case ALGO_BLAKE:
			rc = scanhash_blake256(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done, 14);
			break;

		case ALGO_FRESH:
			rc = scanhash_fresh(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_LYRA2:
			rc = scanhash_lyra2(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_NEOSCRYPT:
			rc = scanhash_neoscrypt(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_NIST5:
			rc = scanhash_nist5(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_PENTABLAKE:
			rc = scanhash_pentablake(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_PLUCK:
			rc = scanhash_pluck(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_SCRYPT:
			rc = scanhash_scrypt(thr_id, work.data, work.target, NULL,
			                      max_nonce, &hashes_done, &tv_start, &tv_end);
			break;

		case ALGO_SCRYPT_JANE:
			rc = scanhash_scrypt_jane(thr_id, work.data, work.target, NULL,
			                      max_nonce, &hashes_done, &tv_start, &tv_end);
			break;

		case ALGO_SKEIN:
			rc = scanhash_skeincoin(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_SKEIN2:
			rc = scanhash_skein2(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_S3:
			rc = scanhash_s3(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_X11:
			rc = scanhash_x11(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_X13:
			rc = scanhash_x13(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_X14:
			rc = scanhash_x14(thr_id, work.data, work.target,
				              max_nonce, &hashes_done);
			break;

		case ALGO_X15:
			rc = scanhash_x15(thr_id, work.data, work.target,
				              max_nonce, &hashes_done);
			break;

		case ALGO_X17:
			rc = scanhash_x17(thr_id, work.data, work.target,
				              max_nonce, &hashes_done);
			break;

		case ALGO_ZR5: {
			rc = scanhash_zr5(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;
		}
		default:
			/* should never happen */
			goto out;
		}

		/* record scanhash elapsed time */
		gettimeofday(&tv_end, NULL);

		if (rc && opt_debug)
			applog(LOG_NOTICE, CL_CYN "found => %08x" CL_GRN " %08x", nonceptr[0], swab32(nonceptr[0])); // data[19]
		if (rc > 1 && opt_debug)
			applog(LOG_NOTICE, CL_CYN "found => %08x" CL_GRN " %08x", nonceptr[2], swab32(nonceptr[2])); // data[21]

		timeval_subtract(&diff, &tv_end, &tv_start);

		if (diff.tv_usec || diff.tv_sec) {
			double dtime = (double) diff.tv_sec + 1e-6 * diff.tv_usec;

			/* hashrate factors for some algos */
			double rate_factor = 1.0;
			switch (opt_algo) {
				case ALGO_JACKPOT:
				case ALGO_QUARK:
					// to stay comparable to other ccminer forks or pools
					rate_factor = 0.5;
					break;
			}

			/* store thread hashrate */
			if (dtime > 0.0) {
				pthread_mutex_lock(&stats_lock);
				thr_hashrates[thr_id] = hashes_done / dtime;
				thr_hashrates[thr_id] *= rate_factor;
				stats_remember_speed(thr_id, hashes_done, thr_hashrates[thr_id], (uint8_t) rc, work.height);
				pthread_mutex_unlock(&stats_lock);
			}
		}

		if (rc > 1)
			work.scanned_to = nonceptr[2];
		else if (rc)
			work.scanned_to = nonceptr[0];
		else {
			work.scanned_to = max_nonce;
			if (opt_debug && opt_benchmark) {
				// to debug nonce ranges
				applog(LOG_DEBUG, "GPU #%d:  ends=%08x range=%llx", device_map[thr_id],
					nonceptr[0], (nonceptr[0] - start_nonce));
			}
		}

		if (check_dups)
			hashlog_remember_scan_range(&work);

		/* output */
		if (!opt_quiet && firstwork_time) {
			format_hashrate(thr_hashrates[thr_id], s);
			applog(LOG_INFO, "GPU #%d: %s, %s",
				device_map[thr_id], device_name[device_map[thr_id]], s);
		}

		/* ignore first loop hashrate */
		if (firstwork_time && thr_id == (opt_n_threads - 1)) {
			double hashrate = 0.;
			pthread_mutex_lock(&stats_lock);
			for (int i = 0; i < opt_n_threads && thr_hashrates[i]; i++)
				hashrate += stats_get_speed(i, thr_hashrates[i]);
			pthread_mutex_unlock(&stats_lock);
			if (opt_benchmark) {
				format_hashrate(hashrate, s);
				applog(LOG_NOTICE, "Total: %s", s);
			}

			// since pool start
			pools[cur_pooln].work_time = (uint32_t) (time(NULL) - firstwork_time);

			// X-Mining-Hashrate
			global_hashrate = llround(hashrate);
		}

		if (firstwork_time == 0)
			firstwork_time = time(NULL);

		/* if nonce found, submit work */
		if (rc && !opt_benchmark) {
			if (!submit_work(mythr, &work))
				break;

			// prevent stale work in solo
			// we can't submit twice a block!
			if (!have_stratum && !have_longpoll) {
				pthread_mutex_lock(&g_work_lock);
				// will force getwork
				g_work_time = 0;
				pthread_mutex_unlock(&g_work_lock);
				continue;
			}

			// second nonce found, submit too (on pool only!)
			if (rc > 1 && work.data[21]) {
				work.data[19] = work.data[21];
				work.data[21] = 0;
				if (opt_algo == ALGO_ZR5) {
					// todo: use + 4..6 index for pok to allow multiple nonces
					work.data[0] = work.data[22]; // pok
					work.data[22] = 0;
				}
				if (!submit_work(mythr, &work))
					break;
			}
		}
	}

out:
	if (opt_debug_threads)
		applog(LOG_DEBUG, "%s() died", __func__);
	tq_freeze(mythr->q);
	return NULL;
}

static void *longpoll_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info *)userdata;
	CURL *curl = NULL;
	char *hdr_path = NULL, *lp_url = NULL;
	bool need_slash = false;
	int pooln;

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		applog(LOG_ERR, "%s() CURL init failed", __func__);
		goto out;
	}

wait_lp_url:
	hdr_path = (char*)tq_pop(mythr->q, NULL); // wait /LP url
	if (!hdr_path)
		goto out;

	pooln = cur_pooln;
	pool_is_switching = false;

start:
	/* full URL */
	if (strstr(hdr_path, "://")) {
		lp_url = hdr_path;
		hdr_path = NULL;
	}
	/* absolute path, on current server */
	else {
		char *copy_start = (*hdr_path == '/') ? (hdr_path + 1) : hdr_path;
		if (rpc_url[strlen(rpc_url) - 1] != '/')
			need_slash = true;

		lp_url = (char*)malloc(strlen(rpc_url) + strlen(copy_start) + 2);
		if (!lp_url)
			goto out;

		sprintf(lp_url, "%s%s%s", rpc_url, need_slash ? "/" : "", copy_start);
	}

	if (have_stratum)
		goto need_reinit;

	applog(LOG_BLUE, "Long-polling enabled on %s", lp_url);

	while (1) {
		json_t *val = NULL, *soval;
		int err;

		// exit on pool switch
		if (pooln != cur_pooln)
			goto need_reinit;

		// not always reset on simple rotate (longpoll 0 -> stratum 1 -> longpoll 0)
		pool_is_switching = false;

		val = json_rpc_call(curl, lp_url, rpc_userpass, rpc_req,
				    false, true, &err);
		if (have_stratum || pooln != cur_pooln) {
			if (val)
				json_decref(val);
			goto need_reinit;
		}
		if (likely(val)) {
			soval = json_object_get(json_object_get(val, "result"), "submitold");
			submit_old = soval ? json_is_true(soval) : false;
			pthread_mutex_lock(&g_work_lock);
			if (work_decode(json_object_get(val, "result"), &g_work)) {
				g_work.pooln = pooln;
				restart_threads();
				if (!opt_quiet) {
					char netinfo[64] = { 0 };
					if (net_diff > 0.) {
						sprintf(netinfo, ", diff %.2f", net_diff);
					}
					applog(LOG_BLUE, "%s detected new block%s", short_url, netinfo);
				}
				g_work_time = time(NULL);
			}
			pthread_mutex_unlock(&g_work_lock);
			json_decref(val);
		} else {
			pthread_mutex_lock(&g_work_lock);
			g_work_time -= LP_SCANTIME;
			pthread_mutex_unlock(&g_work_lock);
			restart_threads();
			if (err != CURLE_OPERATION_TIMEDOUT) {
				have_longpoll = false;
				free(hdr_path);
				free(lp_url);
				lp_url = NULL;
				sleep(opt_fail_pause);
				goto start;
			}
		}
	}

out:
	have_longpoll = false;
	if (opt_debug_threads)
		applog(LOG_DEBUG, "%s() died", __func__);

	free(hdr_path);
	free(lp_url);
	tq_freeze(mythr->q);
	if (curl)
		curl_easy_cleanup(curl);

	return NULL;

need_reinit:
	/* this thread should not die to allow pool switch */
	if (opt_debug_threads)
		applog(LOG_DEBUG, "%s() reinit...", __func__);
	if (hdr_path) free(hdr_path);
	if (lp_url) free(lp_url);
	goto wait_lp_url;
}

static bool stratum_handle_response(char *buf)
{
	json_t *val, *err_val, *res_val, *id_val;
	json_error_t err;
	struct timeval tv_answer, diff;
	bool ret = false;

	val = JSON_LOADS(buf, &err);
	if (!val) {
		applog(LOG_INFO, "JSON decode failed(%d): %s", err.line, err.text);
		goto out;
	}

	res_val = json_object_get(val, "result");
	err_val = json_object_get(val, "error");
	id_val = json_object_get(val, "id");

	if (!id_val || json_is_null(id_val) || !res_val)
		goto out;

	// ignore subscribe late answer (yaamp)
	if (json_integer_value(id_val) < 4)
		goto out;

	gettimeofday(&tv_answer, NULL);
	timeval_subtract(&diff, &tv_answer, &stratum.tv_submit);
	// store time required to the pool to answer to a submit
	stratum.answer_msec = (1000 * diff.tv_sec) + (uint32_t) (0.001 * diff.tv_usec);

	share_result(json_is_true(res_val),
		err_val ? json_string_value(json_array_get(err_val, 1)) : NULL);

	ret = true;
out:
	if (val)
		json_decref(val);

	return ret;
}

static void *stratum_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info *)userdata;
	stratum_ctx *ctx = &stratum;
	int pooln = cur_pooln;
	char *s;

wait_stratum_url:
	stratum.url = (char*)tq_pop(mythr->q, NULL);
	if (!stratum.url)
		goto out;

	ctx->pooln = pooln = cur_pooln;

	if (!pool_is_switching)
		applog(LOG_BLUE, "Starting on %s", stratum.url);

	pool_is_switching = false;

	while (1) {
		int failures = 0;

		if (stratum_need_reset) {
			stratum_need_reset = false;
			if (!stratum.url)
				stratum.url = strdup(rpc_url);
			else
				stratum_disconnect(&stratum);
			/* should not be used anymore, but... */
			if (strcmp(stratum.url, rpc_url)) {
				free(stratum.url);
				stratum.url = strdup(rpc_url);
				applog(LOG_BLUE, "Connection changed to %s", short_url);
			}
		}

		while (!stratum.curl) {
			pthread_mutex_lock(&g_work_lock);
			g_work_time = 0;
			g_work.data[0] = 0;
			pthread_mutex_unlock(&g_work_lock);
			restart_threads();

			if (!stratum_connect(&stratum, stratum.url) ||
			    !stratum_subscribe(&stratum) ||
			    !stratum_authorize(&stratum, rpc_user, rpc_pass))
			{
				stratum_disconnect(&stratum);
				if (opt_retries >= 0 && ++failures > opt_retries) {
					if (opt_pool_failover) {
						applog(LOG_WARNING, "Stratum connect timeout, failover...");
						pool_switch_next();
					} else {
						applog(LOG_ERR, "...terminating workio thread");
						tq_push(thr_info[work_thr_id].q, NULL);
						//workio_abort();
						goto out;
					}
				}
				if (pooln != cur_pooln)
					goto pool_switched;
				if (!opt_benchmark)
					applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
				sleep(opt_fail_pause);
			}
		}

		if (pooln != cur_pooln) goto pool_switched;

		if (stratum.job.job_id &&
		    (!g_work_time || strncmp(stratum.job.job_id, g_work.job_id + 8, 120))) {
			pthread_mutex_lock(&g_work_lock);
			stratum_gen_work(&stratum, &g_work);
			g_work_time = time(NULL);
			if (stratum.job.clean) {
				if (!opt_quiet) {
					if (net_diff > 0.)
						applog(LOG_BLUE, "%s block %d, diff %.2f", algo_names[opt_algo],
							stratum.job.height, net_diff);
					else
						applog(LOG_BLUE, "%s %s block %d", short_url, algo_names[opt_algo],
							stratum.job.height);
				}
				restart_threads();
				if (check_dups)
					hashlog_purge_old();
				stats_purge_old();
			} else if (opt_debug && !opt_quiet) {
					applog(LOG_BLUE, "%s asks job %d for block %d", short_url,
						strtoul(stratum.job.job_id, NULL, 16), stratum.job.height);
			}
			pthread_mutex_unlock(&g_work_lock);
			// not always reset on simple rotate (stratum pool 1 -> longpoll 0 -> stratum 1)
			if (pooln == cur_pooln)
				pool_is_switching = false;
		}
		
		if (pooln != cur_pooln) goto pool_switched;

		if (!stratum_socket_full(&stratum, 120)) {
			applog(LOG_ERR, "Stratum connection timed out");
			s = NULL;
		} else
			s = stratum_recv_line(&stratum);

		if (pooln != cur_pooln) goto pool_switched;

		if (!s) {
			stratum_disconnect(&stratum);
			applog(LOG_ERR, "Stratum connection interrupted");
			continue;
		}
		if (!stratum_handle_method(&stratum, s))
			stratum_handle_response(s);
		free(s);
	}

out:
	if (opt_debug_threads)
		applog(LOG_DEBUG, "%s() died", __func__);

	return NULL;

pool_switched:
	/* this thread should not die on pool switch */
	stratum_disconnect(&(pools[pooln].stratum));
	if (stratum.url) free(stratum.url); stratum.url = NULL;
	if (opt_debug_threads)
		applog(LOG_DEBUG, "%s() reinit...", __func__);
	goto wait_stratum_url;
}

// store current credentials in pools container
void pool_set_creds(int pooln)
{
	struct pool_infos *p = &pools[pooln];

	p->id = pooln;
	// default flags not 0
	p->allow_mininginfo = allow_mininginfo;
	p->allow_gbt = allow_gbt;
	p->check_dups = check_dups;

	snprintf(p->url, sizeof(p->url), "%s", rpc_url);
	snprintf(p->short_url, sizeof(p->short_url), "%s", short_url);
	snprintf(p->user, sizeof(p->user), "%s", rpc_user);
	snprintf(p->pass, sizeof(p->pass), "%s", rpc_pass);

	// init pools options with cmdline ones (if set before -c)
	p->max_diff = opt_max_diff;
	p->max_rate = opt_max_rate;
	p->scantime = opt_scantime;

	if (strlen(rpc_url)) {
		if (!strncasecmp(rpc_url, "stratum", 7))
			p->type = POOL_STRATUM;
		else /* if (!strncasecmp(rpc_url, "http", 4)) */
			p->type = POOL_GETWORK; // todo: or longpoll
		p->status |= POOL_ST_VALID;
	}
	p->status |= POOL_ST_DEFINED;
}

// attributes only set by a json pools config
void pool_set_attr(int pooln, const char* key, char* arg)
{
	struct pool_infos *p = &pools[pooln];

	if (!strcasecmp(key, "name")) {
		snprintf(p->name, sizeof(p->name), "%s", arg);
		return;
	}
	if (!strcasecmp(key, "scantime")) {
		p->scantime = atoi(arg);
		return;
	}
	if (!strcasecmp(key, "max-diff")) {
		p->max_diff = atof(arg);
		return;
	}
	if (!strcasecmp(key, "max-rate")) {
		p->max_rate = atof(arg);
		return;
	}
	if (!strcasecmp(key, "time-limit")) {
		p->time_limit = atoi(arg);
		return;
	}
	if (!strcasecmp(key, "removed")) {
		int removed = atoi(arg);
		if (removed) {
			p->status |= POOL_ST_REMOVED;
		}
		return;
	}
}

// pool switching code
bool pool_switch(int pooln)
{
	int prevn = cur_pooln;
	struct pool_infos *prev = &pools[cur_pooln];
	struct pool_infos* p = NULL;

	// save prev stratum connection infos (struct)
	prev->stratum = stratum;

	if (pooln < num_pools) {
		cur_pooln = pooln;
		p = &pools[cur_pooln];
	} else {
		applog(LOG_ERR, "Switch to inexistant pool %d!", pooln);
		return false;
	}

	// save global attributes
	prev->allow_mininginfo = allow_mininginfo;
	prev->allow_gbt = allow_gbt;
	prev->check_dups = check_dups;
	if (have_longpoll) {
		prev->type = POOL_LONGPOLL;
	}

	pthread_mutex_lock(&g_work_lock);

	free(rpc_user); rpc_user = strdup(p->user);
	free(rpc_pass); rpc_pass = strdup(p->pass);
	free(rpc_userpass);
	rpc_userpass = (char*) malloc(strlen(rpc_user)+strlen(rpc_pass)+2);
	if (rpc_userpass) sprintf(rpc_userpass, "%s:%s", rpc_user, rpc_pass);
	free(rpc_url);  rpc_url = strdup(p->url);
	short_url = p->short_url; // just a pointer, no alloc

	opt_scantime = p->scantime;

	opt_max_diff = p->max_diff;
	opt_max_rate = p->max_rate;
	opt_time_limit = p->time_limit;

	want_stratum = have_stratum = (p->type & POOL_STRATUM) != 0;

	stratum = p->stratum;
	stratum.pooln = cur_pooln;

	// keep mutexes from prev pool (should be moved outside stratum struct)
	stratum.sock_lock = prev->stratum.sock_lock;
	stratum.work_lock = prev->stratum.work_lock;

	pthread_mutex_unlock(&g_work_lock);

	if (prevn != cur_pooln) {
		g_work_time = 0;
		g_work.data[0] = 0;
		pool_is_switching = true;
		stratum_need_reset = true;
		// used to get the pool uptime
		firstwork_time = time(NULL);
		restart_threads();

		// restore flags
		allow_gbt = p->allow_gbt;
		allow_mininginfo = p->allow_mininginfo;
		check_dups = p->check_dups;

		if (want_stratum) {
			// unlock the stratum thread
			tq_push(thr_info[stratum_thr_id].q, strdup(rpc_url));
		}

		// will unlock the longpoll thread on /LP url receive
		want_longpoll = (p->type & POOL_LONGPOLL) || !(p->type & POOL_STRATUM);

		applog(LOG_BLUE, "Switch to pool %d: %s", cur_pooln,
			strlen(p->name) ? p->name : p->short_url);
	}
	return true;
}

// search available pool
int pool_get_first_valid(int startfrom)
{
	int next = 0;
	struct pool_infos *p;
	for (int i=0; i<num_pools; i++) {
		int pooln = (startfrom + i) % num_pools;
		p = &pools[pooln];
		if (!(p->status & POOL_ST_VALID))
			continue;
		if (p->status & (POOL_ST_DISABLED | POOL_ST_REMOVED))
			continue;
		next = pooln;
		break;
	}
	return next;
}

// switch to next available pool
bool pool_switch_next()
{
	if (num_pools > 1) {
		int pooln = pool_get_first_valid(cur_pooln+1);
		return pool_switch(pooln);
	} else {
		// no switch possible
		if (!opt_quiet)
			applog(LOG_DEBUG, "No other pools to try...");
		return false;
	}
}

// seturl from api remote
bool pool_switch_url(char *params)
{
	int prevn = cur_pooln, nextn;
	parse_arg('o', params);
	// cur_pooln modified by parse_arg('o'), get new pool num
	nextn = cur_pooln;
	// and to handle the "hot swap" from current one...
	cur_pooln = prevn;
	if (nextn == prevn)
		return false;
	return pool_switch(nextn);
}

// debug stuff
void pool_dump_infos()
{
	struct pool_infos *p;
	for (int i=0; i<num_pools; i++) {
		p = &pools[i];
		applog(LOG_DEBUG, "POOL %01d: %s USER %s -s %d", i,
			p->short_url, p->user, p->scantime);
	}
}

static void show_version_and_exit(void)
{
	printf("%s v%s\n"
#ifdef WIN32
		"pthreads static %s\n"
#endif
		"%s\n",
		PACKAGE_NAME, PACKAGE_VERSION,
#ifdef WIN32
		PTW32_VERSION_STRING,
#endif
		curl_version());
	proper_exit(EXIT_CODE_OK);
}

static void show_usage_and_exit(int status)
{
	if (status)
		fprintf(stderr, "Try `" PROGRAM_NAME " --help' for more information.\n");
	else
		printf(usage);
	if (opt_algo == ALGO_SCRYPT || opt_algo == ALGO_SCRYPT_JANE) {
		printf(scrypt_usage);
	}
	proper_exit(status);
}

void parse_arg(int key, char *arg)
{
	char *p = arg;
	int v, i;
	double d;

	switch(key) {
	case 'a':
		p = strstr(arg, ":"); // optional factor
		if (p) *p = '\0';
		for (i = 0; i < ARRAY_SIZE(algo_names); i++) {
			if (algo_names[i] && !strcasecmp(arg, algo_names[i])) {
				opt_algo = (enum sha_algos)i;
				break;
			}
		}
		if (i == ARRAY_SIZE(algo_names))
			show_usage_and_exit(1);
		if (p) {
			opt_nfactor = atoi(p + 1);
			if (opt_algo == ALGO_SCRYPT_JANE) {
				free(jane_params);
				jane_params = strdup(p+1);
			}
		}
		if (!opt_nfactor) {
			switch (opt_algo) {
			case ALGO_SCRYPT:      opt_nfactor = 9;  break;
			case ALGO_SCRYPT_JANE: opt_nfactor = 14; break;
			}
		}
		break;
	case 'b':
		p = strstr(arg, ":");
		if (p) {
			/* ip:port */
			if (p - arg > 0) {
				free(opt_api_allow);
				opt_api_allow = strdup(arg);
				opt_api_allow[p - arg] = '\0';
			}
			opt_api_listen = atoi(p + 1);
		}
		else if (arg && strstr(arg, ".")) {
			/* ip only */
			free(opt_api_allow);
			opt_api_allow = strdup(arg);
		}
		else if (arg) {
			/* port or 0 to disable */
			opt_api_listen = atoi(arg);
		}
		break;
	case 1030: /* --api-remote */
		opt_api_remote = 1;
		break;
	case 'B':
		opt_background = true;
		break;
	case 'c': {
		json_error_t err;
		if (opt_config)
			json_decref(opt_config);
#if JANSSON_VERSION_HEX >= 0x020000
		opt_config = json_load_file(arg, 0, &err);
#else
		opt_config = json_load_file(arg, &err);
#endif
		if (!json_is_object(opt_config)) {
			applog(LOG_ERR, "JSON decode of %s failed", arg);
			proper_exit(EXIT_CODE_USAGE);
		}
		break;
	}
	case 'i':
		d = atof(arg);
		v = (uint32_t) d;
		if (v < 0 || v > 31)
			show_usage_and_exit(1);
		{
			int n = 0;
			int ngpus = cuda_num_devices();
			uint32_t last = 0;
			char * pch = strtok(arg,",");
			while (pch != NULL) {
				d = atof(pch);
				v = (uint32_t) d;
				if (v > 7) { /* 0 = default */
					if ((d - v) > 0.0) {
						uint32_t adds = (uint32_t)floor((d - v) * (1 << (v - 8))) * 256;
						gpus_intensity[n] = (1 << v) + adds;
						applog(LOG_INFO, "Adding %u threads to intensity %u, %u cuda threads",
							adds, v, gpus_intensity[n]);
					}
					else if (gpus_intensity[n] != (1 << v)) {
						gpus_intensity[n] = (1 << v);
						applog(LOG_INFO, "Intensity set to %u, %u cuda threads",
							v, gpus_intensity[n]);
					}
				}
				last = gpus_intensity[n];
				n++;
				pch = strtok(NULL, ",");
			}
			while (n < MAX_GPUS)
				gpus_intensity[n++] = last;
		}
		break;
	case 'D':
		opt_debug = true;
		break;
	case 'N':
		v = atoi(arg);
		if (v < 1)
			opt_statsavg = INT_MAX;
		opt_statsavg = v;
		break;
	case 'n': /* --ndevs */
		cuda_print_devices();
		proper_exit(EXIT_CODE_OK);
		break;
	case 'q':
		opt_quiet = true;
		break;
	case 'p':
		free(rpc_pass);
		rpc_pass = strdup(arg);
		pool_set_creds(cur_pooln);
		break;
	case 'P':
		opt_protocol = true;
		break;
	case 'r':
		v = atoi(arg);
		if (v < -1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_retries = v;
		break;
	case 'R':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_fail_pause = v;
		break;
	case 's':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_scantime = v;
		break;
	case 'T':
		v = atoi(arg);
		if (v < 1 || v > 99999)	/* sanity check */
			show_usage_and_exit(1);
		opt_timeout = v;
		break;
	case 't':
		v = atoi(arg);
		if (v < 0 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_n_threads = v;
		break;
	case 'v':
		v = atoi(arg);
		if (v < 0 || v > 8192)	/* sanity check */
			show_usage_and_exit(1);
		opt_vote = (uint16_t)v;
		break;
	case 'm':
		opt_trust_pool = true;
		break;
	case 'u':
		free(rpc_user);
		rpc_user = strdup(arg);
		pool_set_creds(cur_pooln);
		break;
	case 'o':			/* --url */
		if (pools[cur_pooln].type != POOL_UNUSED) {
			// rotate pool pointer
			cur_pooln = (cur_pooln + 1) % MAX_POOLS;
			num_pools = max(cur_pooln+1, num_pools);
			// change some defaults if multi pools
			if (opt_retries == -1) opt_retries = 1;
			if (opt_fail_pause == 30) opt_fail_pause = 5;
		}
		p = strstr(arg, "://");
		if (p) {
			if (strncasecmp(arg, "http://", 7) && strncasecmp(arg, "https://", 8) &&
					strncasecmp(arg, "stratum+tcp://", 14))
				show_usage_and_exit(1);
			free(rpc_url);
			rpc_url = strdup(arg);
			short_url = &rpc_url[(p - arg) + 3];
		} else {
			if (!strlen(arg) || *arg == '/')
				show_usage_and_exit(1);
			free(rpc_url);
			rpc_url = (char*)malloc(strlen(arg) + 8);
			sprintf(rpc_url, "http://%s", arg);
			short_url = &rpc_url[7];
		}
		p = strrchr(rpc_url, '@');
		if (p) {
			char *sp, *ap;
			*p = '\0';
			ap = strstr(rpc_url, "://") + 3;
			sp = strchr(ap, ':');
			if (sp && sp < p) {
				free(rpc_userpass);
				rpc_userpass = strdup(ap);
				free(rpc_user);
				rpc_user = (char*)calloc(sp - ap + 1, 1);
				strncpy(rpc_user, ap, sp - ap);
				free(rpc_pass);
				rpc_pass = strdup(sp + 1);
			} else {
				free(rpc_user);
				rpc_user = strdup(ap);
			}
			// remove user[:pass]@ from rpc_url
			memmove(ap, p + 1, strlen(p + 1) + 1);
			// host:port only
			short_url = ap;
		}
		have_stratum = !opt_benchmark && !strncasecmp(rpc_url, "stratum", 7);
		pool_set_creds(cur_pooln);
		break;
	case 'O':			/* --userpass */
		p = strchr(arg, ':');
		if (!p)
			show_usage_and_exit(1);
		free(rpc_userpass);
		rpc_userpass = strdup(arg);
		free(rpc_user);
		rpc_user = (char*)calloc(p - arg + 1, 1);
		strncpy(rpc_user, arg, p - arg);
		free(rpc_pass);
		rpc_pass = strdup(p + 1);
		pool_set_creds(cur_pooln);
		break;
	case 'x':			/* --proxy */
		if (!strncasecmp(arg, "socks4://", 9))
			opt_proxy_type = CURLPROXY_SOCKS4;
		else if (!strncasecmp(arg, "socks5://", 9))
			opt_proxy_type = CURLPROXY_SOCKS5;
#if LIBCURL_VERSION_NUM >= 0x071200
		else if (!strncasecmp(arg, "socks4a://", 10))
			opt_proxy_type = CURLPROXY_SOCKS4A;
		else if (!strncasecmp(arg, "socks5h://", 10))
			opt_proxy_type = CURLPROXY_SOCKS5_HOSTNAME;
#endif
		else
			opt_proxy_type = CURLPROXY_HTTP;
		free(opt_proxy);
		opt_proxy = strdup(arg);
		pool_set_creds(cur_pooln);
		break;
	case 1001:
		free(opt_cert);
		opt_cert = strdup(arg);
		break;
	case 1002:
		use_colors = false;
		break;
	case 1004:
		opt_autotune = false;
		break;
	case 'l': /* scrypt --launch-config */
		{
			char *last = NULL, *pch = strtok(arg,",");
			int n = 0;
			while (pch != NULL) {
				device_config[n++] = last = strdup(pch);
				pch = strtok(NULL, ",");
			}
			while (n < MAX_GPUS)
				device_config[n++] = last;
		}
		break;
	case 'L': /* scrypt --lookup-gap */
		{
			char *pch = strtok(arg,",");
			int n = 0, last = atoi(arg);
			while (pch != NULL) {
				device_lookup_gap[n++] = last = atoi(pch);
				pch = strtok(NULL, ",");
			}
			while (n < MAX_GPUS)
				device_lookup_gap[n++] = last;
		}
		break;
	case 1050: /* scrypt --interactive */
		{
			char *pch = strtok(arg,",");
			int n = 0, last = atoi(arg);
			while (pch != NULL) {
				device_interactive[n++] = last = atoi(pch);
				pch = strtok(NULL, ",");
			}
			while (n < MAX_GPUS)
				device_interactive[n++] = last;
		}
		break;
	case 1005:
		opt_benchmark = true;
		want_longpoll = false;
		want_stratum = false;
		have_stratum = false;
		break;
	case 1006:
		print_hash_tests();
		proper_exit(EXIT_CODE_OK);
		break;
	case 1003:
		want_longpoll = false;
		break;
	case 1007:
		want_stratum = false;
		break;
	case 1008:
		opt_time_limit = atoi(arg);
		break;
	case 1011:
		allow_gbt = false;
		break;
	case 'S':
	case 1018:
		applog(LOG_INFO, "Now logging to syslog...");
		use_syslog = true;
		if (arg && strlen(arg)) {
			free(opt_syslog_pfx);
			opt_syslog_pfx = strdup(arg);
		}
		break;
	case 1020:
		v = atoi(arg);
		if (v < -1)
			v = -1;
		if (v > (1<<num_cpus)-1)
			v = -1;
		opt_affinity = v;
		break;
	case 1021:
		v = atoi(arg);
		if (v < 0 || v > 5)	/* sanity check */
			show_usage_and_exit(1);
		opt_priority = v;
		break;
	case 1060: // max-temp
		d = atof(arg);
		opt_max_temp = d;
		break;
	case 1061: // max-diff
		d = atof(arg);
		opt_max_diff = d;
		break;
	case 1062: // max-rate
		d = atof(arg);
		p = strstr(arg, "K");
		if (p) d *= 1e3;
		p = strstr(arg, "M");
		if (p) d *= 1e6;
		p = strstr(arg, "G");
		if (p) d *= 1e9;
		opt_max_rate = d;
		break;
	case 'd': // CB
		{
			int ngpus = cuda_num_devices();
			char * pch = strtok (arg,",");
			opt_n_threads = 0;
			while (pch != NULL) {
				if (pch[0] >= '0' && pch[0] <= '9' && pch[1] == '\0')
				{
					if (atoi(pch) < ngpus)
						device_map[opt_n_threads++] = atoi(pch);
					else {
						applog(LOG_ERR, "Non-existant CUDA device #%d specified in -d option", atoi(pch));
						proper_exit(EXIT_CODE_CUDA_NODEVICE);
					}
				} else {
					int device = cuda_finddevice(pch);
					if (device >= 0 && device < ngpus)
						device_map[opt_n_threads++] = device;
					else {
						applog(LOG_ERR, "Non-existant CUDA device '%s' specified in -d option", pch);
						proper_exit(EXIT_CODE_CUDA_NODEVICE);
					}
				}
				// set number of active gpus
				active_gpus = opt_n_threads;
				pch = strtok (NULL, ",");
			}
		}
		break;
	case 'f': // CH - Divisor for Difficulty
		d = atof(arg);
		if (d == 0)	/* sanity check */
			show_usage_and_exit(1);
		opt_difficulty = d;
		break;

	/* PER POOL CONFIG OPTIONS */

	case 1100: /* pool name */
		pool_set_attr(cur_pooln, "name", arg);
		break;
	case 1101: /* pool removed */
		pool_set_attr(cur_pooln, "removed", arg);
		break;
	case 1102: /* pool scantime */
		pool_set_attr(cur_pooln, "scantime", arg);
		break;
	case 1108: /* pool time-limit */
		pool_set_attr(cur_pooln, "time-limit", arg);
		break;
	case 1161: /* pool max-diff */
		pool_set_attr(cur_pooln, "max-diff", arg);
		break;
	case 1162: /* pool max-rate */
		pool_set_attr(cur_pooln, "max-rate", arg);
		break;

	case 'V':
		show_version_and_exit();
	case 'h':
		show_usage_and_exit(0);
	default:
		show_usage_and_exit(1);
	}

	if (use_syslog)
		use_colors = false;
}

/**
 * Parse json config
 */
static bool parse_pool_array(json_t *obj)
{
	size_t idx;
	json_t *p, *val;

	if (!json_is_array(obj))
		return false;

	// array of objects [ {}, {} ]
	json_array_foreach(obj, idx, p)
	{
		if (!json_is_object(p))
			continue;

		for (int i = 0; i < ARRAY_SIZE(cfg_array_keys); i++)
		{
			int opt = -1;
			char *s = NULL;
			if (cfg_array_keys[i].cat != CFG_POOL)
				continue;

			val = json_object_get(p, cfg_array_keys[i].name);
			if (!val)
				continue;

			for (int k = 0; k < ARRAY_SIZE(options); k++)
			{
				const char *alias = cfg_array_keys[i].longname;
				if (alias && !strcasecmp(options[k].name, alias)) {
					opt = k;
					break;
				}
				if (!alias && !strcasecmp(options[k].name, cfg_array_keys[i].name)) {
					opt = k;
					break;
				}
			}
			if (opt == -1)
				continue;

			if (json_is_string(val)) {
				s = strdup(json_string_value(val));
				if (!s)
					continue;

				// applog(LOG_DEBUG, "pool key %s '%s'", options[opt].name, s);
				parse_arg(options[opt].val, s);
				free(s);
			} else {
				// numeric or bool
				char buf[32] = { 0 };
				double d = 0.;
				if (json_is_true(val)) d = 1.;
				else if (json_is_integer(val))
					d = 1.0 * json_integer_value(val);
				else if (json_is_real(val))
					d = json_real_value(val);
				snprintf(buf, sizeof(buf)-1, "%f", d);
				// applog(LOG_DEBUG, "pool key %s '%f'", options[opt].name, d);
				parse_arg(options[opt].val, buf);
			}
		}
	}
	return true;
}

void parse_config(json_t* json_obj)
{
	int i;
	json_t *val;

	if (!json_is_object(json_obj))
		return;

	for (i = 0; i < ARRAY_SIZE(options); i++) {

		if (!options[i].name)
			break;

		if (!strcasecmp(options[i].name, "config"))
			continue;

		val = json_object_get(json_obj, options[i].name);
		if (!val)
			continue;

		if (options[i].has_arg && json_is_string(val)) {
			char *s = strdup(json_string_value(val));
			if (!s)
				continue;
			parse_arg(options[i].val, s);
			free(s);
		}
		else if (options[i].has_arg && json_is_integer(val)) {
			char buf[16];
			sprintf(buf, "%d", (int) json_integer_value(val));
			parse_arg(options[i].val, buf);
		}
		else if (options[i].has_arg && json_is_real(val)) {
			char buf[16];
			sprintf(buf, "%f", json_real_value(val));
			parse_arg(options[i].val, buf);
		}
		else if (!options[i].has_arg) {
			if (json_is_true(val))
				parse_arg(options[i].val, (char*) "");
		}
		else
			applog(LOG_ERR, "JSON option %s invalid",
				options[i].name);
	}

	val = json_object_get(json_obj, "pools");
	if (val && json_typeof(val) == JSON_ARRAY) {
		parse_pool_array(val);
	}
}

static void parse_cmdline(int argc, char *argv[])
{
	int key;

	while (1) {
#if HAVE_GETOPT_LONG
		key = getopt_long(argc, argv, short_options, options, NULL);
#else
		key = getopt(argc, argv, short_options);
#endif
		if (key < 0)
			break;

		parse_arg(key, optarg);
	}
	if (optind < argc) {
		fprintf(stderr, "%s: unsupported non-option argument '%s'\n",
			argv[0], argv[optind]);
		show_usage_and_exit(1);
	}

	parse_config(opt_config);

	if (opt_algo == ALGO_HEAVY && opt_vote == 9999) {
		fprintf(stderr, "%s: Heavycoin hash requires block reward vote parameter (see --vote)\n",
			argv[0]);
		show_usage_and_exit(1);
	}
}

#ifndef WIN32
static void signal_handler(int sig)
{
	switch (sig) {
	case SIGHUP:
		applog(LOG_INFO, "SIGHUP received");
		break;
	case SIGINT:
		signal(sig, SIG_IGN);
		applog(LOG_INFO, "SIGINT received, exiting");
		proper_exit(EXIT_CODE_KILLED);
		break;
	case SIGTERM:
		applog(LOG_INFO, "SIGTERM received, exiting");
		proper_exit(EXIT_CODE_KILLED);
		break;
	}
}
#else
BOOL WINAPI ConsoleHandler(DWORD dwType)
{
	switch (dwType) {
	case CTRL_C_EVENT:
		applog(LOG_INFO, "CTRL_C_EVENT received, exiting");
		proper_exit(EXIT_CODE_KILLED);
		break;
	case CTRL_BREAK_EVENT:
		applog(LOG_INFO, "CTRL_BREAK_EVENT received, exiting");
		proper_exit(EXIT_CODE_KILLED);
		break;
	case CTRL_LOGOFF_EVENT:
		applog(LOG_INFO, "CTRL_LOGOFF_EVENT received, exiting");
		proper_exit(EXIT_CODE_KILLED);
		break;
	case CTRL_SHUTDOWN_EVENT:
		applog(LOG_INFO, "CTRL_SHUTDOWN_EVENT received, exiting");
		proper_exit(EXIT_CODE_KILLED);
		break;
	default:
		return false;
	}
	return true;
}
#endif

int main(int argc, char *argv[])
{
	struct thr_info *thr;
	long flags;
	int i;

	printf("*** ccminer " PACKAGE_VERSION " for nVidia GPUs by tpruvot@github ***\n");
#ifdef _MSC_VER
	printf("    Built with VC++ 2013 and nVidia CUDA SDK 6.5\n\n");
#else
	printf("    Built with the nVidia CUDA SDK 6.5\n\n");
#endif
	printf("  Originally based on Christian Buchner and Christian H. project\n");
	printf("  Include some of the work of djm34, sp, tsiv and klausT.\n\n");
	printf("BTC donation address: 1AJdfCpLWPNoAMDfHF1wD5y8VgKSSTHxPo (tpruvot)\n\n");

	rpc_user = strdup("");
	rpc_pass = strdup("");
	rpc_url = strdup("");

	jane_params = strdup("");

	pthread_mutex_init(&applog_lock, NULL);

	// number of cpus for thread affinity
#if defined(WIN32)
	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	num_cpus = sysinfo.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_CONF)
	num_cpus = sysconf(_SC_NPROCESSORS_CONF);
#elif defined(CTL_HW) && defined(HW_NCPU)
	int req[] = { CTL_HW, HW_NCPU };
	size_t len = sizeof(num_cpus);
	sysctl(req, 2, &num_cpus, &len, NULL, 0);
#else
	num_cpus = 1;
#endif
	if (num_cpus < 1)
		num_cpus = 1;

	for (i = 0; i < MAX_GPUS; i++) {
		device_map[i] = i;
		device_name[i] = NULL;
		device_config[i] = NULL;
		device_backoff[i] = is_windows() ? 12 : 2;
		device_lookup_gap[i] = 1;
		device_batchsize[i] = 1024;
		device_interactive[i] = -1;
		device_texturecache[i] = -1;
		device_singlememory[i] = -1;
	}

	// number of gpus
	active_gpus = cuda_num_devices();
	cuda_devicenames();

	/* parse command line */
	parse_cmdline(argc, argv);

	if (!opt_benchmark && !strlen(rpc_url)) {
		// try default config file (user then binary folder)
		char defconfig[MAX_PATH] = { 0 };
		get_defconfig_path(defconfig, MAX_PATH, argv[0]);
		if (strlen(defconfig)) {
			if (opt_debug)
				applog(LOG_DEBUG, "Using config %s", defconfig);
			parse_arg('c', defconfig);
			parse_cmdline(argc, argv);
		}
	}

	if (!opt_benchmark && !strlen(rpc_url)) {
		fprintf(stderr, "%s: no URL supplied\n", argv[0]);
		show_usage_and_exit(1);
	}

	if (!rpc_userpass) {
		rpc_userpass = (char*)malloc(strlen(rpc_user) + strlen(rpc_pass) + 2);
		if (!rpc_userpass)
			return 1;
		sprintf(rpc_userpass, "%s:%s", rpc_user, rpc_pass);
	}

	/* init stratum data.. */
	memset(&stratum.url, 0, sizeof(stratum));
	pthread_mutex_init(&stratum.sock_lock, NULL);
	pthread_mutex_init(&stratum.work_lock, NULL);

	pthread_mutex_init(&stats_lock, NULL);
	pthread_mutex_init(&g_work_lock, NULL);

	if (opt_debug)
		pool_dump_infos();
	cur_pooln = pool_get_first_valid(0);
	pool_switch(cur_pooln);

	flags = !opt_benchmark && strncmp(rpc_url, "https:", 6)
	      ? (CURL_GLOBAL_ALL & ~CURL_GLOBAL_SSL)
	      : CURL_GLOBAL_ALL;
	if (curl_global_init(flags)) {
		applog(LOG_ERR, "CURL initialization failed");
		return EXIT_CODE_SW_INIT_ERROR;
	}

	if (opt_background) {
#ifndef WIN32
		i = fork();
		if (i < 0) proper_exit(EXIT_CODE_SW_INIT_ERROR);
		if (i > 0) proper_exit(EXIT_CODE_OK);
		i = setsid();
		if (i < 0)
			applog(LOG_ERR, "setsid() failed (errno = %d)", errno);
		i = chdir("/");
		if (i < 0)
			applog(LOG_ERR, "chdir() failed (errno = %d)", errno);
		signal(SIGHUP, signal_handler);
		signal(SIGTERM, signal_handler);
#else
		HWND hcon = GetConsoleWindow();
		if (hcon) {
			// this method also hide parent command line window
			ShowWindow(hcon, SW_HIDE);
		} else {
			HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
			CloseHandle(h);
			FreeConsole();
		}
#endif
	}

#ifndef WIN32
	/* Always catch Ctrl+C */
	signal(SIGINT, signal_handler);
#else
	SetConsoleCtrlHandler((PHANDLER_ROUTINE)ConsoleHandler, TRUE);
	if (opt_priority > 0) {
		DWORD prio = NORMAL_PRIORITY_CLASS;
		switch (opt_priority) {
		case 1:
			prio = BELOW_NORMAL_PRIORITY_CLASS;
			break;
		case 2:
			prio = NORMAL_PRIORITY_CLASS;
			break;
		case 3:
			prio = ABOVE_NORMAL_PRIORITY_CLASS;
			break;
		case 4:
			prio = HIGH_PRIORITY_CLASS;
			break;
		case 5:
			prio = REALTIME_PRIORITY_CLASS;
		}
		SetPriorityClass(GetCurrentProcess(), prio);
	}
#endif
	if (opt_affinity != -1) {
		if (!opt_quiet)
			applog(LOG_DEBUG, "Binding process to cpu mask %x", opt_affinity);
		affine_to_cpu_mask(-1, opt_affinity);
	}
	if (active_gpus == 0) {
		applog(LOG_ERR, "No CUDA devices found! terminating.");
		exit(1);
	}
	if (!opt_n_threads)
		opt_n_threads = active_gpus;

#ifdef HAVE_SYSLOG_H
	if (use_syslog)
		openlog(opt_syslog_pfx, LOG_PID, LOG_USER);
#endif

	work_restart = (struct work_restart *)calloc(opt_n_threads, sizeof(*work_restart));
	if (!work_restart)
		return EXIT_CODE_SW_INIT_ERROR;

	thr_info = (struct thr_info *)calloc(opt_n_threads + 4, sizeof(*thr));
	if (!thr_info)
		return EXIT_CODE_SW_INIT_ERROR;

	/* init workio thread info */
	work_thr_id = opt_n_threads;
	thr = &thr_info[work_thr_id];
	thr->id = work_thr_id;
	thr->q = tq_new();
	if (!thr->q)
		return EXIT_CODE_SW_INIT_ERROR;

	/* start work I/O thread */
	if (pthread_create(&thr->pth, NULL, workio_thread, thr)) {
		applog(LOG_ERR, "workio thread create failed");
		return EXIT_CODE_SW_INIT_ERROR;
	}

	/* longpoll thread */
	longpoll_thr_id = opt_n_threads + 1;
	thr = &thr_info[longpoll_thr_id];
	thr->id = longpoll_thr_id;
	thr->q = tq_new();
	if (!thr->q)
		return EXIT_CODE_SW_INIT_ERROR;

	/* always start the longpoll thread (will wait a tq_push) */
	if (unlikely(pthread_create(&thr->pth, NULL, longpoll_thread, thr))) {
		applog(LOG_ERR, "longpoll thread create failed");
		return EXIT_CODE_SW_INIT_ERROR;
	}

	/* stratum thread */
	stratum_thr_id = opt_n_threads + 2;
	thr = &thr_info[stratum_thr_id];
	thr->id = stratum_thr_id;
	thr->q = tq_new();
	if (!thr->q)
		return EXIT_CODE_SW_INIT_ERROR;

	/* always start the stratum thread (will wait a tq_push) */
	if (unlikely(pthread_create(&thr->pth, NULL, stratum_thread, thr))) {
		applog(LOG_ERR, "stratum thread create failed");
		return EXIT_CODE_SW_INIT_ERROR;
	}

	if (have_stratum && want_stratum) {
		tq_push(thr_info[stratum_thr_id].q, strdup(rpc_url));
	}

#ifdef USE_WRAPNVML
#ifndef WIN32
	/* nvml is currently not the best choice on Windows (only in x64) */
	hnvml = nvml_create();
	if (hnvml)
		applog(LOG_INFO, "NVML GPU monitoring enabled.");
#else
	if (nvapi_init() == 0)
		applog(LOG_INFO, "NVAPI GPU monitoring enabled.");
#endif
	else
		applog(LOG_INFO, "GPU monitoring is not available.");
#endif

	if (opt_api_listen) {
		/* api thread */
		api_thr_id = opt_n_threads + 3;
		thr = &thr_info[api_thr_id];
		thr->id = api_thr_id;
		thr->q = tq_new();
		if (!thr->q)
			return EXIT_CODE_SW_INIT_ERROR;

		/* start stratum thread */
		if (unlikely(pthread_create(&thr->pth, NULL, api_thread, thr))) {
			applog(LOG_ERR, "api thread create failed");
			return EXIT_CODE_SW_INIT_ERROR;
		}
	}

	/* start mining threads */
	for (i = 0; i < opt_n_threads; i++) {
		thr = &thr_info[i];

		thr->id = i;
		thr->gpu.thr_id = i;
		thr->gpu.gpu_id = (uint8_t) device_map[i];
		thr->gpu.gpu_arch = (uint16_t) device_sm[device_map[i]];
		thr->q = tq_new();
		if (!thr->q)
			return EXIT_CODE_SW_INIT_ERROR;

		if (unlikely(pthread_create(&thr->pth, NULL, miner_thread, thr))) {
			applog(LOG_ERR, "thread %d create failed", i);
			return EXIT_CODE_SW_INIT_ERROR;
		}
	}

	applog(LOG_INFO, "%d miner thread%s started, "
		"using '%s' algorithm.",
		opt_n_threads, opt_n_threads > 1 ? "s":"",
		algo_names[opt_algo]);

#ifdef WIN32
	timeBeginPeriod(1); // enable high timer precision (similar to Google Chrome Trick)
#endif

	/* main loop - simply wait for workio thread to exit */
	pthread_join(thr_info[work_thr_id].pth, NULL);

	if (opt_debug)
		applog(LOG_DEBUG, "workio thread dead, exiting.");

	proper_exit(EXIT_CODE_OK);

	return 0;
}
