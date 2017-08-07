#ifndef __MINER_H__
#define __MINER_H__

#include "cpuminer-config.h"

#include <stdbool.h>
#include <inttypes.h>
#include <sys/time.h>
#include <pthread.h>
#include <jansson.h>
#include <curl/curl.h>

#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# ifdef HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif
#ifdef HAVE_ALLOCA_H
# include <alloca.h>
#elif !defined alloca
# ifdef __GNUC__
#  define alloca __builtin_alloca
# elif defined _AIX
#  define alloca __alloca
# elif defined _MSC_VER
#  include <malloc.h>
#  define alloca _alloca
# elif !defined HAVE_ALLOCA
#  ifdef  __cplusplus
extern "C"
#  endif
    void *alloca (size_t);
# endif
#endif

#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#else
enum {
    LOG_ERR,
    LOG_WARNING,
    LOG_NOTICE,
    LOG_INFO,
    LOG_DEBUG,
};
#endif

#undef unlikely
#undef likely
#if defined(__GNUC__) && (__GNUC__ > 2) && defined(__OPTIMIZE__)
#define unlikely(expr) (__builtin_expect(!!(expr), 0))
#define likely(expr) (__builtin_expect(!!(expr), 1))
#else
#define unlikely(expr) (expr)
#define likely(expr) (expr)
#endif

/* Force a compilation error if condition is true */
#define BUILD_BUG_ON(condition) ((void)BUILD_BUG_ON_ZERO(condition))

/* Force a compilation error if condition is true, but also produce a
   result (of value 0 and type size_t), so the expression can be used
   e.g. in a structure initializer (or where-ever else comma expressions
   aren't permitted). */
#define BUILD_BUG_ON_ZERO(e) (sizeof(struct { int:-!!(e); }))
#define BUILD_BUG_ON_NULL(e) ((void *)sizeof(struct { int:-!!(e); }))

# define __same_type(a, b) __builtin_types_compatible_p(typeof(a), typeof(b))
/* &a[0] degrades to a pointer: a different type from an array */
#define __must_be_array(a) BUILD_BUG_ON_ZERO(__same_type((a), &(a)[0]))
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]) + __must_be_array(arr))

#if ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 3))
#define WANT_BUILTIN_BSWAP
#else
#define bswap_32(x) ((((x) << 24) & 0xff000000u) | (((x) << 8) & 0x00ff0000u) \
    | (((x) >> 8) & 0x0000ff00u) | (((x) >> 24) & 0x000000ffu))
#endif

static inline uint32_t swab32(uint32_t v)
{
#ifdef WANT_BUILTIN_BSWAP
    return __builtin_bswap32(v);
#else
    return bswap_32(v);
#endif
}

#ifdef HAVE_SYS_ENDIAN_H
#include <sys/endian.h>
#endif

#if !HAVE_DECL_BE32DEC
static inline uint32_t be32dec(const void *pp)
{
    const uint8_t *p = (uint8_t const *)pp;
    return ((uint32_t)(p[3]) + ((uint32_t)(p[2]) << 8) +
        ((uint32_t)(p[1]) << 16) + ((uint32_t)(p[0]) << 24));
}
#endif

#if !HAVE_DECL_LE32DEC
static inline uint32_t le32dec(const void *pp)
{
    const uint8_t *p = (uint8_t const *)pp;
    return ((uint32_t)(p[0]) + ((uint32_t)(p[1]) << 8) +
        ((uint32_t)(p[2]) << 16) + ((uint32_t)(p[3]) << 24));
}
#endif

#if !HAVE_DECL_BE32ENC
static inline void be32enc(void *pp, uint32_t x)
{
    uint8_t *p = (uint8_t *)pp;
    p[3] = x & 0xff;
    p[2] = (x >> 8) & 0xff;
    p[1] = (x >> 16) & 0xff;
    p[0] = (x >> 24) & 0xff;
}
#endif

#if !HAVE_DECL_LE32ENC
static inline void le32enc(void *pp, uint32_t x)
{
    uint8_t *p = (uint8_t *)pp;
    p[0] = x & 0xff;
    p[1] = (x >> 8) & 0xff;
    p[2] = (x >> 16) & 0xff;
    p[3] = (x >> 24) & 0xff;
}
#endif

#if JANSSON_MAJOR_VERSION >= 2
#define JSON_LOADS(str, err_ptr) json_loads((str), 0, (err_ptr))
#else
#define JSON_LOADS(str, err_ptr) json_loads((str), (err_ptr))
#endif

#define USER_AGENT PACKAGE_NAME "/" PACKAGE_VERSION

//extern void wild_keccak_hash_dbl(uint8_t *md, const uint8_t *in, const uint64_t *scratchpad, uint64_t scr_size);
extern void wild_keccak_hash_dbl(uint8_t *md, const uint8_t *in);
extern int scanhash_wildkeccak(int thr_id, uint32_t *pdata, const uint32_t *ptarget, uint32_t max_nonce, unsigned long *hashes_done);


struct thr_info {
    int		id;
    pthread_t	pth;
    struct thread_q	*q;
};

struct work_restart {
    volatile unsigned long	restart;
    char			padding[128 - sizeof(unsigned long)];
};

extern bool opt_debug;
extern bool opt_protocol;
extern bool opt_redirect;
extern int opt_timeout;
extern bool want_longpoll;
extern bool have_longpoll;
extern bool want_stratum;
extern bool have_stratum;
extern char *opt_cert;
extern char *opt_proxy;
extern long opt_proxy_type;
extern bool use_syslog;
extern pthread_mutex_t applog_lock;
extern struct thr_info *thr_info;
extern int longpoll_thr_id;
extern int stratum_thr_id;
extern struct work_restart *work_restart;
extern const char jsonrpc_2;
extern char rpc2_id[65];

// pad 197340288
#define WILD_KECCAK_SCRATCHPAD_BUFFSIZE (1UL << 28)
struct  __attribute__((__packed__)) scratchpad_hi
{
    unsigned char prevhash[32];
    uint64_t height;
};

#define WILD_KECCAK_ADDENDUMS_ARRAY_SIZE  10



struct __attribute__((__packed__)) addendums_array_entry
{
    struct scratchpad_hi prev_hi;
    uint64_t add_size;
};


struct __attribute__((__packed__)) scratchpad_file_header
{
    struct scratchpad_hi current_hi;
    struct addendums_array_entry add_arr[WILD_KECCAK_ADDENDUMS_ARRAY_SIZE];
    uint64_t scratchpad_size;
};


extern volatile bool stratum_have_work;
extern uint64_t* pscratchpad_buff;
extern volatile uint64_t scratchpad_size;
extern struct scratchpad_hi current_scratchpad_hi;

#define JSON_RPC_LONGPOLL	(1 << 0)
#define JSON_RPC_QUIET_404	(1 << 1)
#define JSON_RPC_IGNOREERR  (1 << 2)

extern void applog(int prio, const char *fmt, ...);
extern json_t *json_rpc_call(CURL *curl, const char *url, const char *userpass,
                             const char *rpc_req, int *curl_err, int flags);
extern char *bin2hex(const unsigned char *p, size_t len);
extern bool hex2bin(unsigned char *p, const char *hexstr, size_t len);
extern size_t hex2bin_len(unsigned char *p, const char *hexstr, size_t len);
extern int timeval_subtract(struct timeval *result, struct timeval *x,
struct timeval *y);
extern bool fulltest(const uint32_t *hash, const uint32_t *target);
extern void diff_to_target(uint32_t *target, double diff);
extern bool rpc2_getfullscratchpad_decode(const json_t *val);


struct work {
    uint32_t data[32];
    uint32_t target[8];

    char *job_id;
    size_t xnonce2_len;
    unsigned char *xnonce2;
};

struct stratum_job {
    char *job_id;
    unsigned char prevhash[32];
    size_t coinbase_size;
    unsigned char *coinbase;
    unsigned char *xnonce2;
    int merkle_count;
    unsigned char **merkle;
    unsigned char version[4];
    unsigned char nbits[4];
    unsigned char ntime[4];
    bool clean;
    double diff;
};

struct stratum_ctx {
    char *url;

    CURL *curl;
    char *curl_url;
    char curl_err_str[CURL_ERROR_SIZE];
    curl_socket_t sock;
    size_t sockbuf_size;
    char *sockbuf;
    pthread_mutex_t sock_lock;

    double next_diff;

    char *session_id;
    size_t xnonce1_size;
    unsigned char *xnonce1;
    size_t xnonce2_size;
    struct stratum_job job;
    struct work work;
    pthread_mutex_t work_lock;
};

bool stratum_keepalived(struct stratum_ctx *sctx , const char *rpc2_id);
bool stratum_socket_full(struct stratum_ctx *sctx, int timeout);
bool stratum_send_line(struct stratum_ctx *sctx, char *s);
char *stratum_recv_line(struct stratum_ctx *sctx);
bool stratum_connect(struct stratum_ctx *sctx, const char *url);
void stratum_disconnect(struct stratum_ctx *sctx);
bool stratum_subscribe(struct stratum_ctx *sctx);
bool stratum_authorize(struct stratum_ctx *sctx, const char *user, const char *pass);
bool stratum_handle_method(struct stratum_ctx *sctx, const char *s);

void UpdateScratchpad(uint32_t threads);
void InitCUDA(uint32_t threads, char **devstrs);
void CUDASetDevice(uint32_t thread_id);

extern bool stratum_getscratchpad(struct stratum_ctx *sctx);
extern bool stratum_request_job(struct stratum_ctx *sctx);

extern bool rpc2_job_decode(const json_t *job, struct work *work);
extern bool rpc2_login_decode(const json_t *val);

struct thread_q;

extern struct thread_q *tq_new(void);
extern void tq_free(struct thread_q *tq);
extern bool tq_push(struct thread_q *tq, void *data);
extern void *tq_pop(struct thread_q *tq, const struct timespec *abstime);
extern void tq_freeze(struct thread_q *tq);
extern void tq_thaw(struct thread_q *tq);

#endif /* __MINER_H__ */
