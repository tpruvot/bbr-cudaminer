/*
* Copyright 2010 Jeff Garzik
* Copyright 2012-2014 pooler
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the Free
* Software Foundation; either version 2 of the License, or (at your option)
* any later version.  See COPYING for more details.
*/

#include "cpuminer-config.h"
#define _GNU_SOURCE

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#if !defined(_WIN64) && !defined(_WIN32)
	#include <sys/mman.h>
#endif
#ifdef WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#if HAVE_SYS_SYSCTL_H
#include <sys/types.h>
#if HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <sys/sysctl.h>
#endif
#endif
#include <jansson.h>
#include <curl/curl.h>
#include "compat.h"
#include "miner.h"

#define PROGRAM_NAME		"cudaminerd"
#define LP_SCANTIME		60
#define JSON_BUF_LEN 345

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

static inline void affine_to_cpu(int id, int cpu) {
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
}
#elif defined(__FreeBSD__) /* FreeBSD specific policy and affinity management */
#include <sys/cpuset.h>
static inline void drop_policy(void)
{
}

static inline void affine_to_cpu(int id, int cpu)
{
	cpuset_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof(cpuset_t), &set);
}
#else
static inline void drop_policy(void)
{
}

static inline void affine_to_cpu(int id, int cpu)
{
}
#endif

enum workio_commands {
	WC_GET_WORK, WC_SUBMIT_WORK,
};

struct workio_cmd {
	enum workio_commands cmd;
	struct thr_info *thr;
	union {
		struct work *work;
	} u;
};

enum mining_algo {
	ALGO_SCRYPT,      /* scrypt(1024,1,1) */
	ALGO_SHA256D,     /* SHA-256d */
	ALGO_KECCAK,      /* Keccak */
	ALGO_HEAVY,       /* Heavy */
	ALGO_QUARK,       /* Quark */
	ALGO_SKEIN,       /* Skein */
	ALGO_SHAVITE3,    /* Shavite3 */
	ALGO_BLAKE,       /* Blake */
	ALGO_X11,         /* X11 */
	ALGO_WILD_KECCAK, /* Boolberry */
	ALGO_CRYPTONIGHT, /* CryptoNight */
};

static const char *algo_names[] = {
	[ALGO_SCRYPT] =      "scrypt",
	[ALGO_SHA256D] =     "sha256d",
	[ALGO_KECCAK] =      "keccak",
	[ALGO_HEAVY] =       "heavy",
	[ALGO_QUARK] =       "quark",
	[ALGO_SKEIN] =       "skein",
	[ALGO_SHAVITE3] =    "shavite3",
	[ALGO_BLAKE] =       "blake",
	[ALGO_X11] =         "x11",
	[ALGO_WILD_KECCAK] = "wildkeccak",
	[ALGO_CRYPTONIGHT] = "cryptonight",
};

bool opt_debug = false;
bool opt_protocol = false;
static bool opt_keepalive = false ;
static bool opt_benchmark = false;
bool opt_redirect = true;
bool want_longpoll = true;
bool have_longpoll = false;
bool want_stratum = true;
bool have_stratum = false;
static bool submit_old = false;
bool use_syslog = false;
static bool opt_background = false;
static bool opt_quiet = false;
static int opt_retries = -1;
static int opt_fail_pause = 10;
const char jsonrpc_2 = true;
int opt_timeout = 0;
static int opt_scantime = 5;
static json_t *opt_config;
static const bool opt_time = true;
static const enum mining_algo opt_algo = ALGO_WILD_KECCAK;
static int opt_n_threads = 1;
static int num_processors;
static char *rpc_url = NULL;
static char *rpc_userpass;
static char *rpc_user, *rpc_pass;
char *opt_cert;
char *opt_proxy;
long opt_proxy_type;
struct thr_info *thr_info;
static int work_thr_id;
int longpoll_thr_id = -1;
int stratum_thr_id = -1;
struct work_restart *work_restart = NULL;
static struct stratum_ctx stratum;
char rpc2_id[65] = "";
static char *rpc2_blob = NULL;
static int rpc2_bloblen = 0;
static uint32_t rpc2_target = 0;
static char *rpc2_job_id = NULL;
unsigned int CUDABlocks = 0, CUDAThreads = 0;

volatile bool stratum_have_work = false;
uint64_t* pscratchpad_buff = NULL;
volatile uint64_t  scratchpad_size = 0;

static char scratchpad_file[PATH_MAX];
static const char cachedir_suffix[] = "boolberry"; /* scratchpad cache saved as ~/.cache/boolberry/scratchpad.bin */

struct scratchpad_hi current_scratchpad_hi;
static struct addendums_array_entry add_arr[WILD_KECCAK_ADDENDUMS_ARRAY_SIZE];
static char last_found_nonce[200];
static time_t prev_save = 0;
static const char * pscratchpad_url = NULL;
static const char * pscratchpad_local_cache = NULL;
char **devstrs = NULL;

pthread_mutex_t applog_lock;
static pthread_mutex_t stats_lock;
static pthread_mutex_t rpc2_job_lock;
static pthread_mutex_t rpc2_login_lock;
static pthread_mutex_t rpc2_getscratchpad_lock;

static unsigned long accepted_count = 0L;
static unsigned long rejected_count = 0L;
static float *thr_hashrates;

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

static char const usage[] =
"Usage: " PROGRAM_NAME " [OPTIONS]\n\
Options:\n\
	-a, --algo=ALGO       specify the algorithm to use\n\
	                      wildkeccak   WildKeccak\n\
	-k  --scratchpad=URL  URL of inital scratchpad file\n\
	-l  --launch-config   threadsxblocks\n\
	-o, --url=URL         URL of mining server\n\
	-O, --userpass=U:P    username:password pair for mining server\n\
	-u, --user=USERNAME   username for mining server\n\
	-p, --pass=PASSWORD   password for mining server\n\
	    --cert=FILE       certificate for mining server using SSL\n\
	-x, --proxy=[PROTOCOL://]HOST[:PORT]  connect through a proxy\n\
	-t, --threads=N       number of miner threads (default: number of processors)\n\
	-K, --keepalive       Send keepalive to prevent timeout (requires pool support)\n\
	-r, --retries=N       number of times to retry if a network call fails\n\
	(default: retry indefinitely)\n\
	-R, --retry-pause=N   time to pause between retries, in seconds (default: 30)\n\
	-T, --timeout=N       timeout for long polling, in seconds (default: none)\n\
	-s, --scantime=N      upper bound on time spent scanning current work when\n\
	                      long polling is unavailable, in seconds (default: 5)\n\
	    --no-longpoll     disable X-Long-Polling support\n\
	    --no-stratum      disable X-Stratum support\n\
	    --no-redirect     ignore requests to change the URL of the mining server\n\
	-q, --quiet           disable per-thread hashmeter output\n\
	-D, --debug           enable debug output\n\
	-P, --protocol-dump   verbose dump of protocol-level activities\n"
#ifdef HAVE_SYSLOG_H
	"\
	-S, --syslog          use system log for output messages\n"
#endif
#ifndef WIN32
	"\
	-B, --background      run the miner in the background\n"
#endif
	"\
	-c, --config=FILE     load a JSON-format configuration file\n\
	-V, --version         display version information and exit\n\
	-h, --help            display this help text and exit\n\
	";

static char const short_options[] =
#ifndef WIN32
	"B"
#endif
#ifdef HAVE_SYSLOG_H
	"S"
#endif
	"a:c:Dhp:Px:Kqr:R:s:t:T:o:u:O:V:k:l:";

static struct option const options[] = {
	{ "algo", 1, NULL, 'a' },
#ifndef WIN32
	{ "background", 0, NULL, 'B' },
#endif
	{ "benchmark", 0, NULL, 1005 },
	{ "scratchpad", 1, NULL, 'k'},
	{ "launch-config", 1, NULL, 'l'},
	{ "cert", 1, NULL, 1001 },
	{ "config", 1, NULL, 'c' },
	{ "debug", 0, NULL, 'D' },
	{ "help", 0, NULL, 'h' },
	{ "keepalive", 0, NULL, 'K' },
	{ "no-longpoll", 0, NULL, 1003 },
	{ "no-redirect", 0, NULL, 1009 },
	{ "no-stratum", 0, NULL, 1007 },
	{ "pass", 1, NULL, 'p' },
	{ "protocol-dump", 0, NULL, 'P' },
	{ "proxy", 1, NULL, 'x' },
	{ "quiet", 0, NULL, 'q' },
	{ "retries", 1, NULL, 'r' },
	{ "retry-pause", 1, NULL, 'R' },
	{ "scantime", 1, NULL, 's' },
#ifdef HAVE_SYSLOG_H
	{ "syslog", 0, NULL, 'S' },
#endif
	{ "threads", 1, NULL, 't' },
	{ "timeout", 1, NULL, 'T' },
	{ "url", 1, NULL, 'o' },
	{ "user", 1, NULL, 'u' },
	{ "userpass", 1, NULL, 'O' },
	{ "version", 0, NULL, 'V' },
	{ 0, 0, 0, 0 }
};

static struct work g_work;
static time_t g_work_time;
static pthread_mutex_t g_work_lock;

static bool rpc2_login(CURL *curl);
static void workio_cmd_free(struct workio_cmd *wc);

json_t *json_rpc2_call_recur(CURL *curl, const char *url,
							 const char *userpass, json_t *rpc_req,
							 int *curl_err, int flags, int recur)
{
	if(recur >= 5) {
		if(opt_debug)
			applog(LOG_DEBUG, "Failed to call rpc command after %i tries", recur);
		return NULL;
	}
	if(!strcmp(rpc2_id, "")) {
		if(opt_debug)
			applog(LOG_DEBUG, "Tried to call rpc2 command before authentication");
		return NULL;
	}
	json_t *params = json_object_get(rpc_req, "params");
	if (params) {
		json_t *auth_id = json_object_get(params, "id");
		if (auth_id) {
			json_string_set(auth_id, rpc2_id);
		}
	}
	json_t *res = json_rpc_call(curl, url, userpass, json_dumps(rpc_req, 0),
		curl_err, flags | JSON_RPC_IGNOREERR);
	if(!res) goto end;
	json_t *error = json_object_get(res, "error");
	if(!error) goto end;
	json_t *message;
	if(json_is_string(error))
		message = error;
	else
		message = json_object_get(error, "message");
	if(!message || !json_is_string(message))
		goto end;
	const char *mes = json_string_value(message);
	if(!strcmp(mes, "Unauthenticated")) {
		pthread_mutex_lock(&rpc2_login_lock);
		rpc2_login(curl);
		sleep(1);
		pthread_mutex_unlock(&rpc2_login_lock);
		return json_rpc2_call_recur(curl, url, userpass, rpc_req,
			curl_err, flags, recur + 1);
	} else if(!strcmp(mes, "Low difficulty share") || !strcmp(mes, "Block expired") || !strcmp(mes, "Invalid job id") || !strcmp(mes, "Duplicate share")) {
		json_t *result = json_object_get(res, "result");
		if(!result) {
			goto end;
		}
		json_object_set(result, "reject-reason", json_string(mes));
	} else {
		applog(LOG_ERR, "json_rpc2.0 error: %s", mes);
		return NULL;
	}
end:
	return res;
}

json_t *json_rpc2_call(CURL *curl, const char *url,
					   const char *userpass, const char *rpc_req,
					   int *curl_err, int flags)
{
						   return json_rpc2_call_recur(curl, url, userpass, JSON_LOADS(rpc_req, NULL),
							   curl_err, flags, 0);
}

static inline void work_free(struct work *w) {
	free(w->job_id);
	free(w->xnonce2);
}

static inline void work_copy(struct work *dest, const struct work *src) {
	memcpy(dest, src, sizeof(struct work));
	if (src->job_id)
		dest->job_id = strdup(src->job_id);
	if (src->xnonce2) {
		dest->xnonce2 = malloc(src->xnonce2_len);
		memcpy(dest->xnonce2, src->xnonce2, src->xnonce2_len);
	}
}

static bool jobj_binary(const json_t *obj, const char *key, void *buf,
						size_t buflen)
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
	if (!hex2bin(buf, hexstr, buflen))
		return false;

	return true;
}


const char* get_json_string_param(const json_t *val, const char* param_name)
{
	json_t *tmp;
	tmp = json_object_get(val, param_name);
	if(!tmp) {
		return NULL;
	}
	return json_string_value(tmp);
}


bool parse_height_info(const json_t *hi_section, struct scratchpad_hi* phi)
{
	if(!phi || !hi_section)
	{
		applog(LOG_ERR, "parse_height_info: wrong params");
		goto err_out;
	}
	json_t *height = json_object_get(hi_section, "height");
	if(!height) {
		applog(LOG_ERR, "JSON inval hi, no height param");
		goto err_out;
	}

	if(!json_is_integer(height))
	{
		applog(LOG_ERR, "JSON inval hi: height is not integer ");
		goto err_out;
	}

	uint64_t hi_h = (uint64_t)json_integer_value(height);
	if(!hi_h)
	{
		applog(LOG_ERR, "JSON inval hi: height is 0");
		goto err_out;
	}

	const char* block_id = get_json_string_param(hi_section, "block_id");
	if(!block_id) {
		applog(LOG_ERR, "JSON inval hi: block_id not found ");
		goto err_out;
	}

	unsigned char prevhash[32] = {};
	size_t len = hex2bin_len(prevhash, block_id, 32);
	if(len != 32)
	{
		applog(LOG_ERR, "JSON inval hi: block_id wrong len %d", len);
		goto err_out;
	}

	phi->height = hi_h;
	memcpy(phi->prevhash, prevhash, 32);

	return true;
err_out:
	return false;
}

void reset_scratchpad(void)
{
	current_scratchpad_hi.height = 0;
	scratchpad_size = 0;
	//unlink(scratchpad_file);
}


bool patch_scratchpad_with_addendum(uint64_t global_add_startpoint, uint64_t* padd_buff, size_t count/*uint64 units*/)
{
	for(int i = 0; i < count; i += 4)
	{
		uint64_t global_offset = (padd_buff[i]%(global_add_startpoint/4))*4;
		for(int j = 0; j != 4; j++)
			pscratchpad_buff[global_offset + j] ^= padd_buff[i + j];
	}
	return true;
}

bool apply_addendum(uint64_t* padd_buff, size_t count/*uint64 units*/)
{
	if(WILD_KECCAK_SCRATCHPAD_BUFFSIZE <= (scratchpad_size + count)*8 )
	{
		applog(LOG_ERR, "!!!!!!! WILD_KECCAK_SCRATCHPAD_BUFFSIZE overflowed !!!!!!!! please increase this constant! ");
		return false;
	}

	if(!patch_scratchpad_with_addendum(scratchpad_size, padd_buff, count))
	{
		applog(LOG_ERR, "patch_scratchpad_with_addendum is broken, resetting scratchpad");
		reset_scratchpad();
		return false;
	}
	for(int k = 0; k != count; k++)
		pscratchpad_buff[scratchpad_size+k] = padd_buff[k];

	scratchpad_size += count;

	return true;
}

bool pop_addendum(struct addendums_array_entry* padd_entry)
{
	if(!padd_entry)
		return false;

	if(!padd_entry->add_size || !padd_entry->prev_hi.height)
	{
		applog(LOG_ERR, "wrong parameters");
		return false;
	}
	patch_scratchpad_with_addendum(scratchpad_size - padd_entry->add_size, &pscratchpad_buff[scratchpad_size - padd_entry->add_size], padd_entry->add_size);
	scratchpad_size = scratchpad_size - padd_entry->add_size;
	memcpy(&current_scratchpad_hi, &padd_entry->prev_hi, sizeof(padd_entry->prev_hi));

	memset(padd_entry, 0, sizeof(struct addendums_array_entry));
	return true;
}

bool revert_scratchpad()
{
	//playback scratchpad addendums for whole add_arr
	size_t i_ = 0;
	size_t i = 0;
	size_t arr_size = ARRAY_SIZE(add_arr);

	for(i_=0; i_ != arr_size; i_++)
	{
		i = arr_size-(i_+1);
		if(!add_arr[i].prev_hi.height)
			continue;
		pop_addendum(&add_arr[i]);
	}
	return true;
}


bool push_addendum_info(struct scratchpad_hi* pprev_hi, uint64_t size /* uint64 units count*/)
{
	//find last free entry
	size_t i = 0;
	size_t arr_size = ARRAY_SIZE(add_arr);

	for(i=0; i != arr_size; i++)
	{
		if(!add_arr[i].prev_hi.height)
			break;
	}

	if(i >= arr_size)
	{//shift array
		memmove(&add_arr[0], &add_arr[1], (arr_size-1)*sizeof(add_arr[0]));
		i = arr_size - 1;
	}
	add_arr[i].prev_hi = *pprev_hi;
	add_arr[i].add_size = size;

	return true;
}

bool addendum_decode(const json_t *addm)
{
	struct scratchpad_hi hi;
	unsigned char prevhash[32];

	json_t* hi_section = json_object_get(addm, "hi");
	if (!hi_section)
	{
		//applog(LOG_ERR, "JSON addms field not found");
		//return false;
		return true;
	}

	if(!parse_height_info(hi_section, &hi))
	{
		return false;
	}

	const char* prev_id_str = get_json_string_param(addm, "prev_id");
	if(!prev_id_str)
	{
		applog(LOG_ERR, "JSON prev_id is not a string");
		return false;
	}
	if(!hex2bin(prevhash, prev_id_str, 32))
	{
		applog(LOG_ERR, "JSON prev_id is not valid hex string");
		return false;
	}


	if(current_scratchpad_hi.height != hi.height -1)
	{
		if(current_scratchpad_hi.height > hi.height -1)
		{
			//skip low scratchpad
			applog(LOG_ERR, "addendum with hi.height=%lld skiped since current_scratchpad_hi.height=%lld", hi.height, current_scratchpad_hi.height);
			return true;
		}
		//TODO: ADD SPLIT HANDLING HERE
		applog(LOG_ERR, "JSON height in addendum-1 (%lld-1) missmatched with current_scratchpad_hi.height(%lld), reverting scratchpad and re-login", hi.height, current_scratchpad_hi.height);
		revert_scratchpad();
		//init re-login
		strcpy(rpc2_id, "");
		return false;
	}

	if(memcmp(prevhash, current_scratchpad_hi.prevhash, 32))
	{
		//TODO: ADD SPLIT HANDLING HERE
		applog(LOG_ERR, "JSON prev_id in addendum missmatched with current_scratchpad_hi.prevhash");
		return false;
	}

	const char* addm_hexstr = get_json_string_param(addm, "addm");
	if(!addm_hexstr)
	{
		applog(LOG_ERR, "JSON prev_id in addendum missmatched with current_scratchpad_hi.prevhash");
		return false;
	}
	size_t add_len = strlen(addm_hexstr);
	if(add_len%64)
	{
		applog(LOG_ERR, "JSON wrong addm hex str len");
		return false;
	}
	uint64_t* padd_buff = malloc(add_len/2);
	if (!padd_buff)
	{
		applog(LOG_ERR, "out of memory, wanted %zu", add_len/2);
		return false;
	}

	if(!hex2bin((unsigned char*)padd_buff, addm_hexstr, add_len/2))
	{
		applog(LOG_ERR, "JSON wrong addm hex str len");
		goto err_out;
	}

	if(!apply_addendum(padd_buff, add_len/16))
	{
		applog(LOG_ERR, "JSON Failed to apply_addendum!");
		goto err_out;
	}
	free(padd_buff);

	push_addendum_info(&current_scratchpad_hi, add_len/16);
	uint64_t old_height = current_scratchpad_hi.height;
	current_scratchpad_hi = hi;


	applog(LOG_INFO, "ADDENDUM APPLIED: %lld --> %lld  %lld blocks added", old_height, current_scratchpad_hi.height, add_len/64);
	return true;

err_out:
	free(padd_buff);
	return false;
}

bool addendums_decode(const json_t *job)
{
	json_t* paddms = json_object_get(job, "addms");
	if (!paddms)
	{
		//applog(LOG_ERR, "JSON addms field not found");
		//return false;
		return true;
	}

	if(!json_is_array(paddms))
	{
		applog(LOG_ERR, "JSON addms field is not array");
		return false;
	}

	unsigned int add_sz = json_array_size(paddms);
	for (int i = 0; i < add_sz; i++)
	{
		json_t *addm = json_array_get(paddms, i);
		if (!addm )
		{
			applog(LOG_ERR, "Internal error: failed to get addm");
			return false;
		}
		if(!addendum_decode(addm))
			return false;
	}

	return true;
}

bool rpc2_job_decode(const json_t *job, struct work *work)
{
	if (!jsonrpc_2) {
		applog(LOG_ERR, "Tried to decode job without JSON-RPC 2.0");
		return false;
	}
	json_t *tmp;
	tmp = json_object_get(job, "job_id");
	if (!tmp) {
		applog(LOG_ERR, "JSON inval job id");
		goto err_out;
	}

	if(!addendums_decode(job))
	{
		applog(LOG_ERR, "JSON failed to process addendums");
		goto err_out;
	}


	const char *job_id = json_string_value(tmp);
	tmp = json_object_get(job, "blob");
	if (!tmp) {
		applog(LOG_ERR, "JSON inval blob");
		goto err_out;
	}
	const char *hexblob = json_string_value(tmp);
	int blobLen = strlen(hexblob);
	if (blobLen % 2 != 0 || ((blobLen / 2) < 40 && blobLen != 0) || (blobLen / 2) > 128)
	{
		applog(LOG_ERR, "JSON invalid blob length");
		goto err_out;
	}
	if (blobLen != 0)
	{
		pthread_mutex_lock(&rpc2_job_lock);
		char *blob = malloc(blobLen / 2);
		if (!hex2bin(blob, hexblob, blobLen / 2))
		{
			applog(LOG_ERR, "JSON inval blob");
			pthread_mutex_unlock(&rpc2_job_lock);
			goto err_out;
		}
		if (rpc2_blob) {
			free(rpc2_blob);
		}
		rpc2_bloblen = blobLen / 2;
		rpc2_blob = malloc(rpc2_bloblen);
		memcpy(rpc2_blob, blob, blobLen / 2);

		free(blob);

		uint32_t target;
		jobj_binary(job, "target", &target, 4);
		if(rpc2_target != target) {
			float hashrate = 0.;
			pthread_mutex_lock(&stats_lock);
			//for (size_t i = 0; i < opt_n_threads; i++)
			//    hashrate += thr_hashrates[i];
			pthread_mutex_unlock(&stats_lock);

			double difficulty = (((double) 0xffffffff) / target);
			applog(LOG_INFO, "Pool set diff to %.0f", difficulty);
			rpc2_target = target;
		}

		if (rpc2_job_id) {
			free(rpc2_job_id);
		}
		rpc2_job_id = strdup(job_id);
		pthread_mutex_unlock(&rpc2_job_lock);
	}
	if(work)
	{
		if (!rpc2_blob) {
			applog(LOG_ERR, "Requested work before work was received");
			goto err_out;
		}
		memcpy(work->data, rpc2_blob, rpc2_bloblen);
		memset(work->target, 0xff, sizeof(work->target));
		//*((uint64_t*)&work->target[6]) = rpc2_target;
		work->target[7] = rpc2_target;

		if (work->job_id)
			free(work->job_id);
		work->job_id = strdup(rpc2_job_id);
		stratum_have_work = true;
	}
	UpdateScratchpad(opt_n_threads);
	return true;

err_out:
	return false;
}

static bool work_decode(const json_t *val, struct work *work) {
	int i;

	if(jsonrpc_2) {
		return rpc2_job_decode(val, work);
	}

	if (unlikely(!jobj_binary(val, "data", work->data, sizeof(work->data)))) {
		applog(LOG_ERR, "JSON inval data");
		goto err_out;
	}
	if (unlikely(!jobj_binary(val, "target", work->target, sizeof(work->target)))) {
		applog(LOG_ERR, "JSON inval target");
		goto err_out;
	}

	for (i = 0; i < ARRAY_SIZE(work->data); i++)
		work->data[i] = le32dec(work->data + i);
	for (i = 0; i < ARRAY_SIZE(work->target); i++)
		work->target[i] = le32dec(work->target + i);

	return true;

err_out: return false;
}

bool rpc2_login_decode(const json_t *val) {
	const char *id;
	const char *s;

	json_t *res = json_object_get(val, "result");
	if(!res) {
		applog(LOG_ERR, "JSON invalid result");
		goto err_out;
	}

	json_t *tmp;
	tmp = json_object_get(res, "id");
	if(!tmp) {
		applog(LOG_ERR, "JSON inval id");
		goto err_out;
	}
	id = json_string_value(tmp);
	if(!id) {
		applog(LOG_ERR, "JSON id is not a string");
		goto err_out;
	}

	strncpy(rpc2_id, id, sizeof(rpc2_id)-1);

	if(opt_debug)
		applog(LOG_DEBUG, "Auth id: %s", id);

	tmp = json_object_get(res, "status");
	if(!tmp) {
		applog(LOG_ERR, "JSON inval status");
		goto err_out;
	}
	s = json_string_value(tmp);
	if(!s) {
		applog(LOG_ERR, "JSON status is not a string");
		goto err_out;
	}
	if(strcmp(s, "OK")) {
		applog(LOG_ERR, "JSON returned status \"%s\"", s);
		return false;
	}


	return true;

err_out: return false;
}


bool rpc2_getfullscratchpad_decode(const json_t *val) {
	const char *status;

	json_t *res = json_object_get(val, "result");
	if(!res) {
		applog(LOG_ERR, "JSON invalid result in rpc2_getfullscratchpad_decode");
		goto err_out;
	}

	//check status
	status = get_json_string_param(res, "status");
	if (!status ) {
		applog(LOG_ERR, "JSON status is not a string");
		goto err_out;
	}

	if(strcmp(status, "OK")) {
		applog(LOG_ERR, "JSON returned status \"%s\"", status);
		goto err_out;
	}

	//parse scratchpad
	const char* scratch_hex = get_json_string_param(res, "scratchpad_hex");
	if (!scratch_hex) {
		applog(LOG_ERR, "JSON scratch_hex is not a string");
		goto err_out;
	}

	size_t len = hex2bin_len((unsigned char*)pscratchpad_buff, scratch_hex, WILD_KECCAK_SCRATCHPAD_BUFFSIZE);
	if (!len)
	{
		applog(LOG_ERR, "JSON scratch_hex is not valid hex");
		goto err_out;
	}

	if (len%8 || len%32)
	{
		applog(LOG_ERR, "JSON scratch_hex is not valid size=%d bytes", len);
		goto err_out;
	}


	//parse hi
	json_t *hi = json_object_get(res, "hi");
	if(!hi) {
		applog(LOG_ERR, "JSON inval hi");
		goto err_out;
	}

	if(!parse_height_info(hi, &current_scratchpad_hi))
	{
		applog(LOG_ERR, "JSON inval hi, failed to parse");
		goto err_out;
	}

	applog(LOG_INFO, "Fetched scratchpad size %d bytes", len);
	scratchpad_size = len/8;

	return true;

err_out: return false;
}

static void share_result(int result, struct work *work, const char *reason)
{
	int i;
	char s[256];
	double hashrate = 0.0;

	pthread_mutex_lock(&stats_lock);
	for(i = 0; i < opt_n_threads; ++i) hashrate += thr_hashrates[i];
	pthread_mutex_unlock(&stats_lock);

	result ? ++accepted_count : ++rejected_count;

	sprintf(s, hashrate >= 1e6 ? "%.0f" : "%.2f", 1e-3 * hashrate);
	applog(LOG_INFO, "accepted: %lu/%lu (%.2f%%), %s khash/s %s", accepted_count, accepted_count + rejected_count,
			100. * accepted_count / (accepted_count + rejected_count), s, result ? "(yay!!!)" : "(booooo)");
}

static bool submit_upstream_work(CURL *curl, struct work *work)
{
	char s[JSON_BUF_LEN], hash[32], *noncestr, *hashhex;
	uint32_t nonce;

	// pass if the previous hash is not the current previous hash
	if(!submit_old && memcmp(work->data + 1 + 8, g_work.data + 1 + 8, 32)) return true;


	noncestr = bin2hex(((const unsigned char*)work->data) + 1, 8);
	strcpy(last_found_nonce, noncestr);
	wild_keccak_hash_dbl((uint8_t *)hash, (uint8_t *)work->data);
	hashhex = bin2hex(hash, 32);
	snprintf(s, JSON_BUF_LEN, "{\"method\": \"submit\", \"params\": {\"id\": \"%s\", \"job_id\": \"%s\", \"nonce\": \"%s\", \"result\": \"%s\"}, \"id\":1}\r\n", rpc2_id, work->job_id, noncestr, hashhex);

	free(hashhex);
	free(noncestr);

	if(unlikely(!stratum_send_line(&stratum, s)))
	{
		applog(LOG_ERR, "submit_upstream_work stratum_send_line failed");
		return(false);
	}

	return(true);
}

static const char *rpc_req = "{\"method\": \"getwork\", \"params\": [], \"id\":0}\r\n";

static bool get_upstream_work(CURL *curl, struct work *work)
{
	json_t *val;
	char s[128];

	snprintf(s, 128, "{\"method\": \"getjob\", \"params\": {\"id\": \"%s\"}, \"id\":1}\r\n", rpc2_id);
	val = json_rpc2_call(curl, rpc_url, rpc_userpass, s, NULL, 0);

	if(val) json_decref(val);
	return(true);
}

static bool rpc2_login(CURL *curl)
{
	char s[JSON_BUF_LEN];
	json_t *val;
	bool rc;

	snprintf(s, JSON_BUF_LEN, "{\"method\": \"login\", \"params\": {\"login\": \"%s\", \"pass\": \"%s\", \"agent\": \"cpuminer-multi/0.1\"}, \"id\": 1}", rpc_user, rpc_pass);
	val = json_rpc_call(curl, rpc_url, rpc_userpass, s, NULL, 0);

	if(!val) return(false);

	rc = rpc2_login_decode(val);
	json_t *result = json_object_get(val, "result");

	if(!result) return(rc);

	applog(LOG_INFO, "Using normal job parsing scenario");

	json_t *job = json_object_get(result, "job");

	if(!rpc2_job_decode(job, &g_work)) return(rc);

	json_decref(val);
	return(rc);
}

static void workio_cmd_free(struct workio_cmd *wc)
{
	if(wc->cmd == WC_SUBMIT_WORK)
	{
		work_free(wc->u.work);
		free(wc->u.work);
	}

	free(wc);
}

static bool workio_get_work(struct workio_cmd *wc, CURL *curl)
{
	struct work *ret_work;
	int failures = 0;

	ret_work = calloc(1, sizeof(*ret_work));

	// obtain new work from bitcoin via JSON-RPC
	while(!get_upstream_work(curl, ret_work))
	{
		if(unlikely((opt_retries >= 0) && (++failures > opt_retries)))
		{
			applog(LOG_ERR, "json_rpc_call failed, terminating workio thread");
			free(ret_work);
			return(false);
		}

		// pause, then restart work-request loop
		applog(LOG_ERR, "getwork failed, retry after %d seconds", opt_fail_pause);
		sleep(opt_fail_pause);
	}

	// send work to requesting thread
	if(!tq_push(wc->thr->q, ret_work)) free(ret_work);

	return(true);
}

static bool workio_submit_work(struct workio_cmd *wc, CURL *curl)
{
	int failures = 0;

	// submit solution to bitcoin via JSON-RPC
	while(!submit_upstream_work(curl, wc->u.work))
	{
		if(unlikely((opt_retries >= 0) && (++failures > opt_retries)))
		{
			applog(LOG_ERR, "...terminating workio thread");
			return(false);
		}

		// pause, then restart work-request loop
		applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
		sleep(opt_fail_pause);
	}

	return(true);
}

static bool workio_login(CURL *curl)
{
	int failures = 0;

	// submit solution to bitcoin via JSON-RPC
	pthread_mutex_lock(&rpc2_login_lock);
	while(!rpc2_login(curl))
	{
		if(unlikely((opt_retries >= 0) && (++failures > opt_retries)))
		{
			applog(LOG_ERR, "...terminating workio thread");
			pthread_mutex_unlock(&rpc2_login_lock);
			return(false);
		}

		/* pause, then restart work-request loop */
		applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
		sleep(opt_fail_pause);
		pthread_mutex_unlock(&rpc2_login_lock);
		pthread_mutex_lock(&rpc2_login_lock);
	}
	pthread_mutex_unlock(&rpc2_login_lock);

	return(true);
}


static void *workio_thread(void *userdata)
{
	struct thr_info *mythr = userdata;
	char ok = true;
	CURL *curl;

	curl = curl_easy_init();

	while(likely(ok))
	{
		struct workio_cmd *wc;

		// wait for workio_cmd sent to us, on our queue

		wc = tq_pop(mythr->q, NULL);
		if(!wc)
			break;

		// process workio_cmd
		if(wc->cmd == WC_GET_WORK) ok = workio_get_work(wc, curl);
		else ok = workio_submit_work(wc, curl);

		workio_cmd_free(wc);
	}

	tq_freeze(mythr->q);
	curl_easy_cleanup(curl);

	return(NULL);
}

static bool get_work(struct thr_info *thr, struct work *work)
{
	struct workio_cmd *wc;
	struct work *work_heap;

	// fill out work request message
	wc = calloc(1, sizeof(*wc));

	wc->cmd = WC_GET_WORK;
	wc->thr = thr;

	/* send work request to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc))
	{
		workio_cmd_free(wc);
		return(false);
	}

	/* wait for response, a unit of work */
	work_heap = tq_pop(thr->q, NULL );
	if(!work_heap) return(false);

	/* copy returned work into storage provided by caller */
	memcpy(work, work_heap, sizeof(*work));
	free(work_heap);

	return(true);
}

static bool submit_work(struct thr_info *thr, const struct work *work_in)
{
	struct workio_cmd *wc;

	/* fill out work request message */
	wc = calloc(1, sizeof(*wc));
	wc->u.work = malloc(sizeof(*work_in));

	wc->cmd = WC_SUBMIT_WORK;
	wc->thr = thr;
	work_copy(wc->u.work, work_in);

	/* send solution to workio thread */
	if(!tq_push(thr_info[work_thr_id].q, wc))
	{
		workio_cmd_free(wc);
		return(false);
	}

	return(true);
}

static inline void stratum_gen_work(struct stratum_ctx *sctx, struct work *work)
{
	pthread_mutex_lock(&sctx->work_lock);
	free(work->job_id);
	memcpy(work, &sctx->work, sizeof(struct work));
	work->job_id = strdup(sctx->work.job_id);
	pthread_mutex_unlock(&sctx->work_lock);
}

static void *miner_thread(void *userdata)
{
	uint32_t max_nonce, end_nonce, *nonceptr;
	struct thr_info *mythr = userdata;
	struct work work = { { 0 } };
	struct sched_param param;
	int thr_id = mythr->id;
	int i;

	end_nonce = 0xffffffffU / opt_n_threads * (thr_id + 1) - 0x20;
	nonceptr = (uint32_t *)(((char *)work.data) + 1);

	CUDASetDevice(thr_id);

	for(;;)
	{
		int rc;
		int64_t max64;
		unsigned long hashes_done;
		struct timeval tv_start, tv_end, diff;

		if (!opt_benchmark)
			while(!scratchpad_size || !stratum_have_work) sleep(1);

		pthread_mutex_lock(&g_work_lock);

		if(((*nonceptr) >= end_nonce) && (!memcmp(((uint8_t *)work.data) + 9, ((uint8_t *)g_work.data) + 9, 71))) stratum_gen_work(&stratum, &g_work);

		if(memcmp(((uint8_t *)work.data) + 9, ((uint8_t *)g_work.data) + 9, 71))
		{
			work_free(&work);
			work_copy(&work, &g_work);
			nonceptr = (uint32_t *)(((char *)work.data) + 1);
			*nonceptr = 0xFFFFFFFFU / opt_n_threads * thr_id;
		}
		else ++(*nonceptr);

		pthread_mutex_unlock(&g_work_lock);
		work_restart[thr_id].restart = 0;

		max64 = LP_SCANTIME * thr_hashrates[thr_id];
		if(max64 <= 0) max64 = 0x1fffffLL;

		if(*nonceptr + max64 > end_nonce) max_nonce = end_nonce;
		else max_nonce = *nonceptr + max64;

		hashes_done = 0;

		gettimeofday(&tv_start, NULL);
		rc = scanhash_wildkeccak(thr_id, work.data, work.target, max_nonce, &hashes_done);
		gettimeofday(&tv_end, NULL);

		timeval_subtract(&diff, &tv_end, &tv_start);
		if(likely(diff.tv_usec || diff.tv_sec))
		{
			float tmp = hashes_done / (diff.tv_sec + (diff.tv_usec * 1e-6));
			pthread_mutex_lock(&stats_lock);
			thr_hashrates[thr_id] = tmp;
			pthread_mutex_unlock(&stats_lock);
		}

		applog(LOG_INFO, "GPU #%d: %s: %lu hashes, %.2f kh/s", thr_id, devstrs[thr_id], hashes_done, 1e-3 * thr_hashrates[thr_id]);

		if(rc && !submit_work(mythr, &work)) break;
	}

	tq_freeze(mythr->q);
	return(NULL);
}


static void restart_threads(void)
{
	int i;

	for (i = 0; i < opt_n_threads; i++)
		work_restart[i].restart = 1;
}

bool store_scratchpad_to_file(bool do_fsync)
{
	FILE *fp;
	char file_name_buff[PATH_MAX];
	int ret;

	if(opt_algo != ALGO_WILD_KECCAK || !scratchpad_size) return true;

	snprintf(file_name_buff, sizeof(file_name_buff), "%s.tmp", pscratchpad_local_cache);
	unlink(file_name_buff);
	fp = fopen(file_name_buff, "wbx");
	if(fp == NULL)
	{
		applog(LOG_INFO, "failed to create file %s: %s", file_name_buff, strerror(errno));
		return false;
	}

	struct scratchpad_file_header sf = {0};
	memcpy(&sf.add_arr[0], &add_arr[0], sizeof(sf.add_arr));
	sf.current_hi = current_scratchpad_hi;
	sf.scratchpad_size = scratchpad_size;



	if ((fwrite(&sf, sizeof(sf), 1, fp) != 1) ||
		(fwrite(pscratchpad_buff, 8, scratchpad_size, fp) != scratchpad_size)) {
			applog(LOG_ERR, "failed to write file %s: %s", file_name_buff, strerror(errno));
			fclose(fp);
			unlink(file_name_buff);
			return false;
	}
	fflush(fp);
	/*if (do_fsync) {
		if (fsync(fileno(fp)) == -1) {
			applog(LOG_ERR, "failed to fsync file %s: %s", file_name_buff, strerror(errno));
			fclose(fp);
			unlink(file_name_buff);
			return false;
		}
	}*/
	if (fclose(fp) == EOF) {
		applog(LOG_ERR, "failed to write file %s: %s", file_name_buff, strerror(errno));
		unlink(file_name_buff);
		return false;
	}
	ret = rename(file_name_buff, pscratchpad_local_cache);
	if (ret == -1) {
		applog(LOG_ERR, "failed to rename %s to %s: %s",
			file_name_buff, pscratchpad_local_cache, strerror(errno));
		unlink(file_name_buff);
		return false;
	}
	applog(LOG_DEBUG, "saved scratchpad to %s (%zu+%zu bytes)", pscratchpad_local_cache,
		sizeof(struct scratchpad_file_header), (size_t)scratchpad_size * 8);
	return true;
}

/* TODO: repetitive error+log spam handling */
bool load_scratchpad_from_file(const char *fname)
{
	FILE *fp;
	long flen;
	size_t szhi = sizeof(struct scratchpad_file_header);

	fp = fopen(fname, "rb");
	if (fp == NULL)
	{
		if (errno != ENOENT) {
			applog(LOG_ERR, "failed to load %s: %s", fname, strerror(errno));
		}
		return false;
	}

	struct scratchpad_file_header fh = {0};
	if ((fread(&fh, sizeof(fh), 1, fp) != 1))
	{
		applog(LOG_ERR, "read error from %s: %s", fname, strerror(errno));
		fclose(fp);
		return false;
	}


	if ((fh.scratchpad_size*8 > (WILD_KECCAK_SCRATCHPAD_BUFFSIZE)) ||(fh.scratchpad_size%4))
	{
		applog(LOG_ERR, "file %s size invalid (%" PRIu64 "), max=%zu",
			fname, fh.scratchpad_size*8, WILD_KECCAK_SCRATCHPAD_BUFFSIZE);
		fclose(fp);
		return false;
	}

	if (fread(pscratchpad_buff, 8,  fh.scratchpad_size, fp) != fh.scratchpad_size)
	{
		applog(LOG_ERR, "read error from %s: %s", fname, strerror(errno));
		fclose(fp);
		return false;
	}
	scratchpad_size = fh.scratchpad_size;
	current_scratchpad_hi = fh.current_hi;
	memcpy(&add_arr[0], &fh.add_arr[0], sizeof(fh.add_arr));
	flen = (long)scratchpad_size*8;

	applog(LOG_DEBUG, "loaded scratchpad %s (%ld bytes), height=%" PRIu64, fname, flen, current_scratchpad_hi.height);
	fclose(fp);
	prev_save = time(NULL);
	return true;
}


bool dump_scratchpad_to_file_debug()
{
	FILE *fp;
	char file_name_buff[1000] = {0};
	snprintf(file_name_buff, sizeof(file_name_buff), "scratchpad_%" PRIu64 "_%s.scr",
		current_scratchpad_hi.height, last_found_nonce);

	/* do not bother rewriting if it exists already */


	fp=fopen(file_name_buff, "w");
	if(fp == NULL)
	{
		applog(LOG_INFO, "failed to open file %s: %s", file_name_buff, strerror(errno));
		return false;
	}
	if (fwrite(pscratchpad_buff, 8, scratchpad_size, fp) != scratchpad_size) {
		applog(LOG_ERR, "failed to write file %s: %s", file_name_buff, strerror(errno));
		fclose(fp);
		return false;
	}
	if (fclose(fp) == EOF) {
		applog(LOG_ERR, "failed to write file %s: %s", file_name_buff, strerror(errno));
		return false;
	}

	fclose(fp);
	return true;
}


static bool try_mkdir_chdir(const char *dirn)
{
	if (chdir(dirn) == -1) {
		if (errno == ENOENT) {
#if defined(_WIN32) || defined(_WIN64)
			if (mkdir(dirn) == -1) {
#else
			if (mkdir(dirn, 0700) == -1) {
#endif
				applog(LOG_ERR, "mkdir failed: %s", strerror(errno));
				return false;
			}
			if (chdir(dirn) == -1) {
				applog(LOG_ERR, "chdir failed: %s", strerror(errno));
				return false;
			}
		} else {
			applog(LOG_ERR, "chdir failed: %s", strerror(errno));
			return false;
		}
	}
	return true;
}


static bool stratum_handle_response(char *buf) {
	json_t *val, *err_val, *res_val, *id_val;
	json_error_t err;
	bool ret = false;
	bool valid = false;

	val = JSON_LOADS(buf, &err);
	if (!val) {
		applog(LOG_INFO, "JSON decode failed(%d): %s", err.line, err.text);
		goto out;
	}

	res_val = json_object_get(val, "result");
	err_val = json_object_get(val, "error");
	id_val = json_object_get(val, "id");

	if (!id_val || json_is_null(id_val) /*|| !res_val*/)
		goto out;

	if(jsonrpc_2)
	{
		json_t *status = NULL;
		if(res_val)
			status = json_object_get(res_val, "status");
		if(status) {
			const char *s = json_string_value(status);
			if (!strcmp(s, "KEEPALIVED")) {
				applog(LOG_INFO, "Keepalive receveid");
				goto out;
			}
			valid = !strcmp(s, "OK") && json_is_null(err_val);
		} else {
			valid = json_is_null(err_val);
		}

		if(err_val && !json_is_null(err_val) )
		{
			const char* perr_msg = get_json_string_param(err_val, "message");
			if(perr_msg && !strcmp(perr_msg, "Unauthenticated"))
			{
				applog(LOG_ERR, "Response returned \"Unauthenticated\", need to relogin");
				err_val = json_object_get(err_val, "message");
				valid = false;
			}
			else if(perr_msg && !strcmp(perr_msg, "Low difficulty share"))
			{
				//applog(LOG_ERR, "Dump scratchpad file");
				//dump_scrstchpad_to_file();
			}

			strcpy(rpc2_id, "");
			stratum_have_work = false;
			restart_threads();
		}
	} else {
		valid = res_val && json_is_true(res_val);
	}

	share_result(valid, NULL,
		err_val ? (jsonrpc_2 ? json_string_value(err_val) : json_string_value(json_array_get(err_val, 1))) : NULL );

	ret = true;
out: if (val)
		 json_decref(val);

	 return ret;
}

static void *stratum_thread(void *userdata) {
	struct thr_info *mythr = userdata;
	char *s;

	stratum.url = tq_pop(mythr->q, NULL );
	if (!stratum.url)
		goto out;
	applog(LOG_INFO, "Starting Stratum on %s", stratum.url);

	while (1) {
		int failures = 0;

		while (!stratum.curl) {
			pthread_mutex_lock(&g_work_lock);
			g_work_time = 0;
			pthread_mutex_unlock(&g_work_lock);
			restart_threads();

			if (!stratum_connect(&stratum, stratum.url)
				|| !stratum_subscribe(&stratum)
				|| !stratum_authorize(&stratum, rpc_user, rpc_pass)) {
					stratum_disconnect(&stratum);
					if (opt_retries >= 0 && ++failures > opt_retries) {
						applog(LOG_ERR, "...terminating workio thread");
						tq_push(thr_info[work_thr_id].q, NULL );
						goto out;
					}
					applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
					sleep(opt_fail_pause);
			}
		}

		if(!strcmp(rpc2_id, ""))
		{
			if (opt_debug)
				applog(LOG_DEBUG, "disconnecting...");
			stratum_disconnect(&stratum);
			//not logged in, try to relogin
			if (opt_debug)
				applog(LOG_DEBUG, "Re-connect and relogin...");
			if(!stratum_connect(&stratum, stratum.url) || !stratum_authorize(&stratum, rpc_user, rpc_pass))
			{
				stratum_disconnect(&stratum);
				applog(LOG_ERR, "Failed...retry after %d seconds", opt_fail_pause);
				sleep(opt_fail_pause);
			}
		}

		if(opt_algo == ALGO_WILD_KECCAK && !scratchpad_size)
		{
			if(!stratum_getscratchpad(&stratum))
			{
				stratum_disconnect(&stratum);
				applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
				sleep(opt_fail_pause);
			}
			store_scratchpad_to_file(false);
			prev_save = time(NULL);

			if(!stratum_request_job(&stratum))
			{
				stratum_disconnect(&stratum);
				applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
				sleep(opt_fail_pause);
			}
		}
		if(opt_algo == ALGO_WILD_KECCAK)
		{
		  /* save every 12 hours */
		  if ((time(NULL) - prev_save) > 12*3600)
		  {
			store_scratchpad_to_file(false);
			prev_save = time(NULL);
		  }
		}

		if (jsonrpc_2) {
			if (stratum.work.job_id && (!g_work_time || strcmp(stratum.work.job_id, g_work.job_id)))
			{
				pthread_mutex_lock(&g_work_lock);
				stratum_gen_work(&stratum, &g_work);
				time(&g_work_time);
				pthread_mutex_unlock(&g_work_lock);
				applog(LOG_INFO, "Stratum detected new block");
				restart_threads();
			}
		} else {
			if (stratum.job.job_id
				&& (!g_work_time
				|| strcmp(stratum.job.job_id, g_work.job_id))) {
					pthread_mutex_lock(&g_work_lock);
					stratum_gen_work(&stratum, &g_work);
					time(&g_work_time);
					pthread_mutex_unlock(&g_work_lock);
					if (stratum.job.clean) {
						applog(LOG_INFO, "Stratum detected new block");
						restart_threads();
					}
			}
		}

		if (opt_keepalive && !stratum_socket_full(&stratum, 90)) {
			applog(LOG_INFO, "Keepalive send....");
			stratum_keepalived(&stratum, rpc2_id);
			s = NULL;
		}
		if (!stratum_socket_full(&stratum, 300)) {
			applog(LOG_ERR, "Stratum connection timed out");
			s = NULL;
		} else
			s = stratum_recv_line(&stratum);
		if (!s) {
			stratum_disconnect(&stratum);
			applog(LOG_ERR, "Stratum connection interrupted");
			continue;
		}
		if (!stratum_handle_method(&stratum, s))
			stratum_handle_response(s);
		free(s);
	}

out: return NULL ;
}

static void show_version_and_exit(void) {
	printf(PACKAGE_STRING "\n built on " __DATE__ "\n features:"
#if defined(__i386__)
		" i386"
#endif
#if defined(__x86_64__)
		" x86_64"
#endif
#if defined(__i386__) || defined(__x86_64__)
		" SSE2"
#endif
#if defined(__x86_64__) && defined(USE_AVX)
		" AVX"
#endif
#if defined(__x86_64__) && defined(USE_AVX2)
		" AVX2"
#endif
#if defined(__x86_64__) && defined(USE_XOP)
		" XOP"
#endif
#if defined(__arm__) && defined(__APCS_32__)
		" ARM"
#if defined(__ARM_ARCH_5E__) || defined(__ARM_ARCH_5TE__) || \
	defined(__ARM_ARCH_5TEJ__) || defined(__ARM_ARCH_6__) || \
	defined(__ARM_ARCH_6J__) || defined(__ARM_ARCH_6K__) || \
	defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_6T2__) || \
	defined(__ARM_ARCH_6Z__) || defined(__ARM_ARCH_6ZK__) || \
	defined(__ARM_ARCH_7__) || \
	defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_7R__) || \
	defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
		" ARMv5E"
#endif
#if defined(__ARM_NEON__)
		" NEON"
#endif
#endif
		"\n");

	printf("%s\n", curl_version());
#ifdef JANSSON_VERSION
	printf("libjansson %s\n", JANSSON_VERSION);
#endif
	exit(0);
}

static void show_usage_and_exit(int status) {
	if (status)
		fprintf(stderr,
		"Try `" PROGRAM_NAME " --help' for more information.\n");
	else
		printf(usage);
	exit(status);
}

static void parse_arg(int key, char *arg) {
	char *p;
	int v, i;

	switch (key) {
	case 'a':
		fprintf(stderr, "parsing algo: %s\n", arg);
		for (i = 0; i < ARRAY_SIZE(algo_names); i++) {
			if (algo_names[i] && !strcmp(arg, algo_names[i])) {
				//opt_algo = i;
				break;
			}
		}
		if (i == ARRAY_SIZE(algo_names))
		{
			printf("algo name not found: %s", arg);
			show_usage_and_exit(1);
		}
		break;
	case 'k':
		pscratchpad_url = arg;
		break;
	case 'l':
		sscanf(arg, "%ux%u", &CUDABlocks, &CUDAThreads);
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
			exit(1);
		}
		break;
			  }
	case 'q':
		opt_quiet = true;
		break;
	case 'D':
		opt_debug = true;
		break;
	case 'p':
		free(rpc_pass);
		rpc_pass = strdup(arg);
		break;
	case 'P':
		opt_protocol = true;
		break;
	case 'r':
		v = atoi(arg);
		if (v < -1 || v > 9999) /* sanity check */
			show_usage_and_exit(1);
		opt_retries = v;
		break;
	case 'R':
		v = atoi(arg);
		if (v < 1 || v > 9999) /* sanity check */
			show_usage_and_exit(1);
		opt_fail_pause = v;
		break;
	case 's':
		v = atoi(arg);
		if (v < 1 || v > 9999) /* sanity check */
			show_usage_and_exit(1);
		opt_scantime = v;
		break;
	case 'T':
		v = atoi(arg);
		if (v < 1 || v > 99999) /* sanity check */
			show_usage_and_exit(1);
		opt_timeout = v;
		break;
	case 't':
		v = atoi(arg);
		if (v < 1 || v > 9999) /* sanity check */
			show_usage_and_exit(1);
		opt_n_threads = v;
		break;
	case 'u':
		free(rpc_user);
		rpc_user = strdup(arg);
		break;
	case 'o': /* --url */
		p = strstr(arg, "://");
		if (p) {
			if (strncasecmp(arg, "http://", 7)
				&& strncasecmp(arg, "https://", 8)
				&& strncasecmp(arg, "stratum+tcp://", 14))
				show_usage_and_exit(1);
			free(rpc_url);
			rpc_url = strdup(arg);
		} else {
			if (!strlen(arg) || *arg == '/')
				show_usage_and_exit(1);
			free(rpc_url);
			rpc_url = malloc(strlen(arg) + 8);
			sprintf(rpc_url, "http://%s", arg);
		}
		p = strrchr(rpc_url, '@');
		if (p) {
			char *sp, *ap;
			*p = '\0';
			ap = strstr(rpc_url, "://") + 3;
			sp = strchr(ap, ':');
			if (sp) {
				free(rpc_userpass);
				rpc_userpass = strdup(ap);
				free(rpc_user);
				rpc_user = calloc(sp - ap + 1, 1);
				strncpy(rpc_user, ap, sp - ap);
				free(rpc_pass);
				rpc_pass = strdup(sp + 1);
			} else {
				free(rpc_user);
				rpc_user = strdup(ap);
			}
			memmove(ap, p + 1, strlen(p + 1) + 1);
		}
		have_stratum = !opt_benchmark && !strncasecmp(rpc_url, "stratum", 7);
		break;
	case 'O': /* --userpass */
		p = strchr(arg, ':');
		if (!p)
			show_usage_and_exit(1);
		free(rpc_userpass);
		rpc_userpass = strdup(arg);
		free(rpc_user);
		rpc_user = calloc(p - arg + 1, 1);
		strncpy(rpc_user, arg, p - arg);
		free(rpc_pass);
		rpc_pass = strdup(p + 1);
		break;
	case 'x': /* --proxy */
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
		break;
	case 1001:
		free(opt_cert);
		opt_cert = strdup(arg);
		break;
	case 1005:
		opt_benchmark = true;
		want_longpoll = false;
		want_stratum = false;
		have_stratum = false;
		break;
	case 1003:
		want_longpoll = false;
		break;
	case 1007:
		want_stratum = false;
		break;
	case 1009:
		opt_redirect = false;
		break;
	case 'S':
		use_syslog = true;
		break;
	case 'V':
		show_version_and_exit();
	case 'h':
		show_usage_and_exit(0);
	default:
		show_usage_and_exit(1);
	}
}

static void parse_config(void) {
	int i;
	json_t *val;

	if (!json_is_object(opt_config))
		return;

	for (i = 0; i < ARRAY_SIZE(options); i++) {
		if (!options[i].name)
			break;
		if (!strcmp(options[i].name, "config"))
			continue;

		val = json_object_get(opt_config, options[i].name);
		if (!val)
			continue;

		if (options[i].has_arg && json_is_string(val)) {
			char *s = strdup(json_string_value(val));
			if (!s)
				break;
			parse_arg(options[i].val, s);
			free(s);
		} else if (!options[i].has_arg && json_is_true(val))
			parse_arg(options[i].val, "");
		else
			applog(LOG_ERR, "JSON option %s invalid", options[i].name);
	}
}

static void parse_cmdline(int argc, char *argv[]) {
	int key;

	while (1) {
#if HAVE_GETOPT_LONG
		key = getopt_long(argc, argv, short_options, options, NULL );
#else
		key = getopt(argc, argv, short_options);
#endif
		if (key < 0)
			break;

		parse_arg(key, optarg);
	}
	if (optind < argc) {
		fprintf(stderr, "%s: unsupported non-option argument '%s'\n", argv[0],
			argv[optind]);
		show_usage_and_exit(1);
	}

	parse_config();
}

#ifndef WIN32
static void signal_handler(int sig) {
	switch (sig) {
	case SIGHUP:
		applog(LOG_INFO, "SIGHUP received");
		break;
	case SIGINT:
		applog(LOG_INFO, "SIGINT received, exiting");
		exit(0);
		break;
	case SIGTERM:
		applog(LOG_INFO, "SIGTERM received, exiting");
		exit(0);
		break;
	}
}
#endif

size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	size_t written = fwrite(ptr, size, nmemb, stream);
	return written;
}

bool download_inital_scratchpad(const char* path_to, const char* url)
{
	applog(LOG_INFO, "Downloading scratchpad....");
	CURL *curl;
	FILE *fp;
	CURLcode res;
	char curl_error_buff[CURL_ERROR_SIZE] = {0};
	curl = curl_easy_init();
	if (curl) {
		fp = fopen(path_to,"wb");
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error_buff);
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
		res = curl_easy_perform(curl);
		if(CURLE_OK != res)
		{
			applog(LOG_ERR, "Failed to download file, error: %s", curl_error_buff);
		}else
		{
			applog(LOG_INFO, "Scratchpad downloaded OK.");
		}

		/* always cleanup */
		curl_easy_cleanup(curl);
		fclose(fp);
		if(CURLE_OK != res)
		{
			return false;
		}
	}else
	{
		applog(LOG_INFO, "Failed to curl_easy_init.");
		return false;
	}
	return true;
}

#if !defined(_WIN64) && !defined(_WIN32)
void GetScratchpad(void)
{
	size_t sz = WILD_KECCAK_SCRATCHPAD_BUFFSIZE;
	const char *phome_var_name = "HOME";
	char cachedir[PATH_MAX];

	if(!getenv(phome_var_name))
	{
		applog(LOG_ERR, "$%s not set", phome_var_name);
		exit(1);
	}
	else if(!try_mkdir_chdir(getenv(phome_var_name)))
	{
		exit(1);
	}

	if(!try_mkdir_chdir(".cache")) exit(1);

	if(!try_mkdir_chdir(cachedir_suffix)) exit(1);

	if (getcwd(cachedir, sizeof(cachedir) - 22) == NULL)
	{
		applog(LOG_ERR, "getcwd failed: %s", strerror(errno));
		exit(1);
	}

	snprintf(scratchpad_file, sizeof(scratchpad_file), "%s/scratchpad.bin", cachedir);
	pscratchpad_local_cache = scratchpad_file;

	applog(LOG_DEBUG, "wildkeccak scratchpad cache %s", pscratchpad_local_cache);

	pscratchpad_buff = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, 0, 0);
	if(pscratchpad_buff == MAP_FAILED)
	{
		applog(LOG_INFO, "hugetlb not available");
		pscratchpad_buff = malloc(sz);
		if(!pscratchpad_buff)
		{
			applog(LOG_ERR, "Scratchpad allocation failed");
			exit(1);
		}
	}
	else
	{
		applog(LOG_INFO, "using hugetlb");
	}
	madvise(pscratchpad_buff, sz, MADV_RANDOM | MADV_WILLNEED | MADV_HUGEPAGE);
	mlock(pscratchpad_buff, sz);

	if(!load_scratchpad_from_file(pscratchpad_local_cache))
	{
		if(!pscratchpad_url)
		{
			applog(LOG_ERR, "Scratchpad URL not set. Please specify correct scratchpad url by -k or --scratchpad option");
			exit(1);
		}
		if(!download_inital_scratchpad(pscratchpad_local_cache, pscratchpad_url))
		{
			applog(LOG_ERR, "Scratchpad not found and not downloaded. Please specify correct scratchpad url by -k or --scratchpad  option");
			exit(1);
		}
		if(!load_scratchpad_from_file(pscratchpad_local_cache))
		{
			applog(LOG_ERR, "Failed to load scratchpad data after downloading, probably broken scratchpad link, please restart miner with correct inital scratcpad link(-k or --scratchpad )");
			unlink(pscratchpad_local_cache);
			exit(1);
		}
	}
}

#else

void GetScratchpad(void)
{
	size_t sz = WILD_KECCAK_SCRATCHPAD_BUFFSIZE;
	const char* phome_var_name = "LOCALAPPDATA";
	char cachedir[PATH_MAX];

	if(!getenv(phome_var_name))
	{
		applog(LOG_ERR, "$%s not set", phome_var_name);
		exit(1);
	}
	else if(!try_mkdir_chdir(getenv(phome_var_name)))
	{
		exit(1);
	}

	if(!try_mkdir_chdir(".cache")) exit(1);

	if(!try_mkdir_chdir(cachedir_suffix)) exit(1);

	if (getcwd(cachedir, sizeof(cachedir) - 22) == NULL)
	{
		applog(LOG_ERR, "getcwd failed: %s", strerror(errno));
		exit(1);
	}

	snprintf(scratchpad_file, sizeof(scratchpad_file), "%s/scratchpad.bin", cachedir);
	pscratchpad_local_cache = scratchpad_file;

	applog(LOG_DEBUG, "wildkeccak scratchpad cache %s", pscratchpad_local_cache);

	pscratchpad_buff = malloc(sz);
	if(!pscratchpad_buff)
	{
		applog(LOG_ERR, "Scratchpad allocation failed");
		exit(1);
	}

	if(!load_scratchpad_from_file(pscratchpad_local_cache))
	{
		if(!pscratchpad_url)
		{
			applog(LOG_ERR, "Scratchpad URL not set. Please specify correct scratchpad url by -k or --scratchpad option");
			exit(1);
		}
		if(!download_inital_scratchpad(pscratchpad_local_cache, pscratchpad_url))
		{
			applog(LOG_ERR, "Scratchpad not found and not downloaded. Please specify correct scratchpad url by -k or --scratchpad  option");
			exit(1);
		}
		if(!load_scratchpad_from_file(pscratchpad_local_cache))
		{
			applog(LOG_ERR, "Failed to load scratchpad data after downloading, probably broken scratchpad link, please restart miner with correct inital scratcpad link(-k or --scratchpad )");
			unlink(pscratchpad_local_cache);
			exit(1);
		}
	}
}

#endif
int main(int argc, char *argv[]) {
	struct thr_info *thr;
	long flags;
	int i;

	rpc_user = strdup("");
	rpc_pass = strdup("");

	tzset();

	pthread_mutex_init(&applog_lock, NULL );
	pthread_mutex_init(&stats_lock, NULL );
	pthread_mutex_init(&g_work_lock, NULL );
	pthread_mutex_init(&rpc2_job_lock, NULL );
	pthread_mutex_init(&stratum.sock_lock, NULL );
	pthread_mutex_init(&stratum.work_lock, NULL );

	/* parse command line */
	parse_cmdline(argc, argv);

	if(!CUDABlocks | !CUDAThreads)
	{
		CUDABlocks = 60;
		CUDAThreads = 130;
	}

	applog(LOG_INFO, "Using JSON-RPC 2.0");

	GetScratchpad();

	if(!rpc_url && !opt_benchmark)
	{
		fprintf(stderr, "%s: no URL supplied\n", argv[0]);
		show_usage_and_exit(1);
	}

	if (!rpc_userpass && !opt_benchmark)
	{
		rpc_userpass = malloc(strlen(rpc_user) + strlen(rpc_pass) + 2);
		if(!rpc_userpass) return(1);
		sprintf(rpc_userpass, "%s:%s", rpc_user, rpc_pass);
	}

	flags = CURL_GLOBAL_ALL & ~CURL_GLOBAL_SSL;
	if(curl_global_init(flags))
	{
		applog(LOG_ERR, "CURL initialization failed");
		return(1);
	}

#ifndef WIN32
	if (opt_background) {
		i = fork();
		if (i < 0)
			exit(1);
		if (i > 0)
			exit(0);
		i = setsid();
		if (i < 0)
			applog(LOG_ERR, "setsid() failed (errno = %d)", errno);
		i = chdir("/");
		if (i < 0)
			applog(LOG_ERR, "chdir() failed (errno = %d)", errno);
		signal(SIGHUP, signal_handler);
		signal(SIGINT, signal_handler);
		signal(SIGTERM, signal_handler);
	}
#endif

#ifdef HAVE_SYSLOG_H
	if (use_syslog)
		openlog("cpuminer", LOG_PID, LOG_USER);
#endif

	work_restart = calloc(opt_n_threads, sizeof(*work_restart));
	thr_info = calloc(opt_n_threads + 3, sizeof(*thr));
	thr_hashrates = (float *)calloc(opt_n_threads, sizeof(float));
	devstrs = (char **)malloc(sizeof(char *) * opt_n_threads);

	InitCUDA(opt_n_threads, devstrs);

	/* init workio thread info */
	work_thr_id = opt_n_threads;
	thr = &thr_info[work_thr_id];
	thr->id = work_thr_id;
	thr->q = tq_new();
	if (!thr->q)
		return 1;

	/* start work I/O thread */
	if (pthread_create(&thr->pth, NULL, workio_thread, thr)) {
		applog(LOG_ERR, "workio thread create failed");
		return 1;
	}

	if (want_stratum && !opt_benchmark)
	{
		/* init stratum thread info */
		stratum_thr_id = opt_n_threads + 2;
		thr = &thr_info[stratum_thr_id];
		thr->id = stratum_thr_id;
		thr->q = tq_new();
		if (!thr->q)
			return 1;

		/* start stratum thread */
		if (unlikely(pthread_create(&thr->pth, NULL, stratum_thread, thr))) {
			applog(LOG_ERR, "stratum thread create failed");
			return 1;
		}

		if (have_stratum)
			tq_push(thr_info[stratum_thr_id].q, strdup(rpc_url));
	}

	/* start mining threads */
	for (i = 0; i < opt_n_threads; i++) {
		thr = &thr_info[i];

		thr->id = i;
		thr->q = tq_new();
		if (!thr->q)
			return 1;

		if (unlikely(pthread_create(&thr->pth, NULL, miner_thread, thr))) {
			applog(LOG_ERR, "thread %d create failed", i);
			return 1;
		}
	}

	applog(LOG_INFO, "%d miner threads started, "
		"using '%s' algorithm.", opt_n_threads, algo_names[opt_algo]);

	/* main loop - simply wait for workio thread to exit */
	pthread_join(thr_info[work_thr_id].pth, NULL );

	applog(LOG_INFO, "workio thread dead, exiting.");

	return 0;
}
