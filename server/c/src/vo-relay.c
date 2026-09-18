/* Atari 2600 Video Olympics netplay relay -- C implementation.
 *
 * A transliteration of ../../vo_relay_server.py, which stays the canonical
 * reference.  NOTE that the Python's module docstring is inherited from
 * Combat and is stale in four places -- it still says ver=2, an 8-byte wire
 * frame, `(avail >> 3) << 3` and a joystick-nibble input byte.  The comments
 * here are written from what that file's CODE does.  Function names and their order in this file match the
 * Python methods one-for-one so that a future protocol change can be diffed
 * mechanically rather than re-derived; tools/server_diff.py holds the two
 * implementations to byte-for-byte equality on the wire and in the log.
 *
 * THE RELAY IS DUMB.  It pairs two consoles, hands each a role and a seed,
 * and forwards frames between them without ever looking inside one -- except
 * to read the tick and checksum out of an INPUT record so it can say whether
 * the two consoles ever disagreed.  Every piece of synchronisation lives in
 * the ROM, where it has to.
 *
 *     frame := len(1) type(1) payload(len-1)
 * `len` counts type+payload, so a frame is never zero length and a bad one is
 * visible on sight.
 *
 *     C->S  $01 HELLO   ver(1)=3, tv(1) 0=NTSC 1=PAL (2=SECAM is refused),
 *                       name 2-8 of A-Z0-9
 *     C->S  $02 LIST                      (reserved; the ROM auto-pairs)
 *     S->C  $03 LOBBY   count(1), then name(8) status(1) per entry
 *     C->S  $04 JOIN    name               (reserved)
 *     S->C  $05 START   role(1) seed_lo seed_hi delay(1) variation(1) opp(8)
 *     C<->C $06 INPUT   tick(1) window(3 ticks x 3 bytes) crc(1) check(1)
 *     C<->C $08 STATE
 *     C->S  $0A BYE
 *     S->C  $0B PEER_LEFT   pad(14)
 *     C->S  $0C PING  ->  S->C $0D PONG
 *
 * EVERY FRAME A CONSOLE CAN RECEIVE DURING PLAY IS EXACTLY SIXTEEN BYTES ON
 * THE WIRE, where Combat used eight.  The 2600 has no buffer to reassemble a
 * split frame in, so the console reads `avail AND $F0` bytes and a partial
 * frame is simply not read this time.  Session frames -- HELLO, START, LOBBY
 * -- are NOT padded; the boot bank reads them one at a time and knows each
 * one's length.
 *
 * Deliberate differences from the Python, all documented in README.md:
 *   - --fixed-seed and --lobby-keepalive exist here only, as test hooks;
 *   - an https:// --lobby-url is refused at startup rather than failing late;
 *   - the by_name table is bounded (the Python's dict is not; see name_set).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "lobby.h"

/* ---- protocol constants ----------------------------------------------- */

enum {
    T_HELLO = 0x01, T_LIST  = 0x02, T_LOBBY = 0x03, T_JOIN  = 0x04,
    T_START = 0x05, T_INPUT = 0x06, T_CRC   = 0x07, T_STATE = 0x08,
    T_RESYNC = 0x09, T_BYE  = 0x0A, T_PEER_LEFT = 0x0B, T_PING = 0x0C,
    T_PONG  = 0x0D
};

/* Forwarded to the partner verbatim, never interpreted. */
#define IS_RELAY_TYPE(t) \
    ((t) == T_INPUT || (t) == T_CRC || (t) == T_STATE || (t) == T_RESYNC)

/* Every frame a console can receive DURING PLAY is this long on the wire, and
 * it is a POWER OF TWO on purpose: the console reads (avail AND $F0) and
 * leaves a partial frame where it is, which makes misframing structurally
 * unreachable.
 *
 * SIXTEEN, WHERE COMBAT USED EIGHT.  A paddle position is a whole byte where a
 * joystick was a nibble, and every record carries a WINDOW of three ticks, so
 * a tick costs three bytes on the wire -- paddle A, paddle B, and the switches
 * and triggers.  Payload is tick(1) + 3*3 + crc(1) + check(1) = 12, padded to
 * 14, and 16 on the wire with the length and type bytes.
 *
 * The second paddle is carried from day one even though only paddle A is wired
 * up: four-player Quadrapong and Foozpong then cost a mixer change rather than
 * a protocol version bump, and the record was going to be padded to 16 anyway.
 */
#define GAME_FRAME              16
#define GAME_PAYLOAD            (GAME_FRAME - 2)

/* Offsets inside an INPUT payload: tick(1) window(3*3) crc(1) check(1).  Keep
 * in step with IR_* in src/vodefs.inc.  The checksum's offset has moved twice
 * -- once when the record started carrying a WINDOW of three ticks instead of
 * one byte, and again when a tick's input grew from a joystick nibble to a
 * paddle byte -- so it is named, not counted.  The relay reads only the tick
 * and the checksum. */
#define IR_TICK                 0
#define IR_IN                   1
#define IR_CRC                  10
#define IR_CHK                  11
/* log_crc's guard is `len(payload) > IR_CRC` here, where Combat and Dragster
 * use `>= 6` -- one byte laxer, because it does not require the check byte.
 * Transcribed from each game's own Python, not unified. */
#define CRC_MIN_PAYLOAD         (IR_CRC + 1)

#define PROTO                   3   /* 2 put the television standard between
                                     * the version byte and the name.  3
                                     * widened a tick's input from one byte to
                                     * three, which is another SHAPE change: an
                                     * older relay would pair the wrong
                                     * checksums. */
#define TV_NTSC                 0
#define TV_PAL                  1
#define TV_SECAM                2

#define DEFAULT_DELAY           2   /* ticks; 3 frames a tick, so ~100 ms */
#define VARIATION_MIN           0
#define VARIATION_MAX           49
/* THE VARIATION WHITELIST.
 *
 * Video Olympics has fifty variations and the manual numbers them 1-50; the
 * ROM's own cell, $96, holds 0-49, so a manual game N is $96 = N-1.
 *
 * Two of them are single-player -- Robot Pong, and Pong against the computer
 * -- and are meaningless over a network.  Twenty-six are four-player.  Until
 * the second paddle slot is wired up at both ends, the twenty-two two-player
 * games are the ones that work, and those are the ones SELECT walks.
 *
 * It lives here as well as in the ROM because the START frame carries the
 * variation the match begins on, and a relay that could start a pair on Robot
 * Pong would be handing them a game one of them cannot play. */
static const uint8_t TWO_PLAYER[] = {
    2, 3, 8, 9, 12, 13, 18, 19, 22, 23, 24, 25, 26, 27,
    34, 35, 38, 39, 42, 43, 44, 45
};
#define DEFAULT_VARIATION       2       /* TWO_PLAYER[0]: manual game 3,
                                         * two-player Pong */

#define MAX_TX_BACKLOG    (64 * 1024)
/* The Python's Client.send() appends without limit: only flush() enforces the
 * backlog, and it does so on what is LEFT after one send().  So tx legitimately
 * overshoots MAX_TX_BACKLOG within a single event-loop iteration, and this
 * array has to cover the whole overshoot or the C would silently discard bytes
 * the Python would have delivered.
 *
 * The bound, derived rather than guessed:
 *   - service() does ONE recv() of at most 4096 bytes, and rx holds at most a
 *     200-byte partial frame on entry, so at most 4296 bytes are parsed;
 *   - the smallest legal frame is 2 bytes, so that is at most 2148 frames;
 *   - the largest reply a 2-byte frame can provoke is a LOBBY answering LIST:
 *     2 + 1 + 8*9 = 75 bytes.  (A relayed game frame is only 8 or 16.)
 *   - so at most 2148 * 75 = 161,100 bytes are appended before flush() runs,
 *     and flush() drops the client when what remains exceeds MAX_TX_BACKLOG.
 * tx therefore peaks at 65,536 + 161,100 = 226,636 bytes.  256 KiB covers it
 * with room to spare, and client_alloc() deliberately does not touch the array
 * so the pages stay unfaulted until a client actually backs up. */
#define TX_CAP            (256 * 1024)
#define RX_CAP                  4352    /* 4096 recv + 200 residue, rounded  */
#define MAX_CLIENTS             64      /* global established-connection cap */
#define MAX_CONNECTIONS_PER_IP  8       /* generous: FujiNets share a NAT    */
#define HELLO_TIMEOUT           60.0    /* seconds to identify before drop   */
#define STATS_INTERVAL          60.0    /* seconds between stats lines       */
#define SWEEP_INTERVAL          5.0
#define CRC_SLOTS               16      /* Python keeps <= 9; see log_crc    */
#define FRAME_MAX               256     /* outbound scratch; largest = 201   */
#define NAME_SLOTS              (MAX_CLIENTS * 4)

/* ---- types ------------------------------------------------------------- */

/* One client's tick -> crc log.  An INSERTION-ORDERED map, never a hash:
 * eviction is by AGE, and evicting the lowest key instead of the oldest
 * produced false CRC MISMATCH reports (PORTING.md 4.19). */
typedef struct {
    uint64_t key;                   /* lap * 256 + tick */
    uint8_t  crc;
} crcent_t;

typedef struct {
    int      in_use;
    int      fd;                    /* -1 once dropped                      */
    uint32_t gen;                   /* bumped on free; other half of the tag */
    uint32_t ip;                    /* network order, for the per-IP cap     */
    char     ipstr[INET_ADDRSTRLEN];/* addr[0]                               */
    unsigned peer_port;             /* addr[1]                               */
    /* The rx and tx BUFFERS live outside this struct, in rxbuf[]/txbuf[].
     * Keeping a quarter of a megabyte per client in here would make
     * sizeof(client_t) enormous, and then any walk of clients[] -- the one in
     * main() that sets every fd to -1, for instance -- would touch a byte
     * every 260 KiB across 16 MiB of .bss.  With transparent huge pages on
     * (`[always]` is the default on this machine) that faults in 2 MiB at a
     * time and makes essentially the whole .bss resident before a single
     * console has connected.  Split out and madvised MADV_NOHUGEPAGE, only
     * the 4 KiB pages a client actually writes become resident. */
    uint32_t rx_len;
    uint32_t tx_len;
    char     name[9];               /* "" until HELLO                        */
    int      has_name;
    int      tv;
    int      partner;               /* client index, -1 = none               */
    int      role;
    double   born;
    int      dead;                  /* set by drop(); slot freed at end of
                                     * the iteration, so a service() loop can
                                     * keep parsing exactly as Python does    */
    uint32_t armed;                 /* current epoll event mask              */
    crcent_t crc[CRC_SLOTS];
    int      ncrc;
    int      have_last_tick;
    uint8_t  last_tick;
    uint64_t lap;
} client_t;

/* by_name maps a name to the client that claimed it.  A second HELLO on one
 * connection leaves the old entry behind pointing at this client, exactly as
 * the Python's dict does; the entry is never dereferenced, it only keeps the
 * name taken and counts towards curplayers. */
typedef struct {
    char     name[9];
    int      ci;
    uint32_t gen;
} nameent_t;

typedef struct {
    unsigned matches;
    unsigned crc_ok;
    unsigned crc_bad;
    unsigned bad_frames;
    unsigned tx_drops;
    unsigned hello_timeouts;
    unsigned ip_rejects;
} stats_t;

/* ---- globals ----------------------------------------------------------- */

static client_t  clients[MAX_CLIENTS];
/* Demand-paged, and deliberately NOT huge-page backed: see client_t. */
static uint8_t   rxbuf[MAX_CLIENTS][RX_CAP];
static uint8_t   txbuf[MAX_CLIENTS][TX_CAP];
/* self.clients is a dict keyed by socket: ACCEPT-ORDERED.  idle() and
 * send_lobby() iterate it, and send_lobby slices the first 8 named entries,
 * so that order IS the lobby a console displays.  A slot array iterates in
 * slot order and slots get reused, which is a different order; this is the
 * insertion-ordered index that keeps the two the same. */
static int       order[MAX_CLIENTS];
static int       n_order;

static nameent_t by_name[NAME_SLOTS];
static int       n_by_name;

static int       ep = -1;
static int       sigpipe_fd[2] = { -1, -1 };
static volatile sig_atomic_t stop_requested;

static stats_t   stats;
static lobby_t  *lob;

static const char *g_host  = "0.0.0.0";
static int    g_port       = 9600;
static int    g_delay      = DEFAULT_DELAY;
static int    g_variation  = DEFAULT_VARIATION;
static int    g_any_variation;
static int    g_fixed_seed;

#define TAG_LISTEN UINT64_C(0xFFFFFFFFFFFFFFFF)
#define TAG_SIG    UINT64_C(0xFFFFFFFFFFFFFFFE)

static void drop(int ci);
static void handle(int ci, uint8_t ftype, const uint8_t *payload, size_t plen);

/* ---- logging ----------------------------------------------------------- */

/* print(("%s " % time.strftime("%H:%M:%S")) + msg, flush=True).
 *
 * Local time-of-day, no date and no level, on STDOUT -- not the Intellivision
 * relay's logging.basicConfig format on stderr.  Line-buffered so each record
 * is one write(2): test/run_rig.sh reads the log while the server still runs.
 * The Lobby publisher thread calls this too, hence the lock. */
void relay_log(const char *level, const char *fmt, ...)
{
    time_t now;
    struct tm tm;
    char ts[16];
    va_list ap;

    (void)level;                /* the 2600 log has no level field */
    now = time(NULL);
    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);

    flockfile(stdout);
    fprintf(stdout, "%s ", ts);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
    funlockfile(stdout);
}

#define LOG(...) relay_log("INFO", __VA_ARGS__)

/* ---- small helpers ----------------------------------------------------- */

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* frame(ftype, payload) -> writes into buf, returns total bytes. */
static size_t frame_build(uint8_t *buf, uint8_t ftype,
                          const uint8_t *payload, size_t plen)
{
    buf[0] = (uint8_t)(plen + 1);       /* len counts type+payload */
    buf[1] = ftype;
    if (plen)
        memcpy(buf + 2, payload, plen);
    return plen + 2;
}

static void pad_name(uint8_t *dst8, const char *name)
{
    size_t n = strlen(name);
    if (n > 8)
        n = 8;
    memset(dst8, 0, 8);
    memcpy(dst8, name, n);
}

/* c.name or "?" -- the Python idiom in three log lines. */
static const char *name_or_q(int ci)
{
    return clients[ci].has_name ? clients[ci].name : "?";
}

/* repr() of the str that failed the name check.
 *
 * The name reaching here has been through decode("ascii","replace"), so every
 * byte >= 0x80 is one U+FFFD; Python's repr renders that literally, as UTF-8.
 * C0 controls become \xNN (\t \n \r have short forms), and the quote is a
 * single quote unless the string contains one and no double quote. */
static const char *py_repr(const uint8_t *s, size_t n)
{
    /* A rejected name is at most 197 bytes and a U+FFFD costs three, so this
     * never truncates on any input the framing can deliver. */
    static char b[640];
    size_t o = 0;
    char q = '\'';

    for (size_t i = 0; i < n; i++)
        if (s[i] == '\'') {
            q = '"';
            for (size_t j = 0; j < n; j++)
                if (s[j] == '"')
                    q = '\'';
            break;
        }

    b[o++] = q;
    for (size_t i = 0; i < n && o < sizeof b - 8; i++) {
        uint8_t ch = s[i];
        if (ch >= 0x80) {               /* U+FFFD, as UTF-8 */
            b[o++] = (char)0xEF; b[o++] = (char)0xBF; b[o++] = (char)0xBD;
        } else if (ch == (uint8_t)q || ch == '\\') {
            b[o++] = '\\'; b[o++] = (char)ch;
        } else if (ch == '\t') {
            b[o++] = '\\'; b[o++] = 't';
        } else if (ch == '\n') {
            b[o++] = '\\'; b[o++] = 'n';
        } else if (ch == '\r') {
            b[o++] = '\\'; b[o++] = 'r';
        } else if (ch < 0x20 || ch == 0x7F) {
            o += (size_t)snprintf(b + o, sizeof b - o, "\\x%02x", ch);
        } else {
            b[o++] = (char)ch;
        }
    }
    b[o++] = q;
    b[o] = '\0';
    return b;
}

static uint64_t rng_state;

/* random.randrange(1, 0x10000).  Python's Mersenne Twister sequence is not
 * reproducible here, which is why the differential normalizes the seed and
 * --fixed-seed exists. */
static uint16_t next_seed(void)
{
    uint16_t v;
    if (g_fixed_seed)
        return (uint16_t)g_fixed_seed;
    do {
        uint64_t z;
        rng_state += UINT64_C(0x9E3779B97F4A7C15);
        z = rng_state;
        z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
        v = (uint16_t)((z ^ (z >> 31)) & 0xFFFF);
    } while (v == 0);
    return v;
}

/* random.randrange(10) */
static int next_digit(void)
{
    uint64_t z;
    rng_state += UINT64_C(0x9E3779B97F4A7C15);
    z = rng_state;
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return (int)((z ^ (z >> 31)) % 10u);
}

/* The start variation has to be one a pair of remote players can actually
 * play; see TWO_PLAYER. */
static int is_two_player(int v)
{
    for (size_t i = 0; i < sizeof TWO_PLAYER / sizeof TWO_PLAYER[0]; i++)
        if (TWO_PLAYER[i] == v)
            return 1;
    return 0;
}

static uint64_t tag_of(int ci)
{
    return ((uint64_t)clients[ci].gen << 32) | (uint32_t)ci;
}

/* ---- the accept-ordered client index ----------------------------------- */

static void order_add(int ci)
{
    if (n_order < MAX_CLIENTS)
        order[n_order++] = ci;
}

static void order_del(int ci)
{
    for (int i = 0; i < n_order; i++) {
        if (order[i] == ci) {
            memmove(&order[i], &order[i + 1],
                    (size_t)(n_order - i - 1) * sizeof order[0]);
            n_order--;
            return;
        }
    }
}

/* ---- by_name ----------------------------------------------------------- */

static int name_taken(const char *name)
{
    for (int i = 0; i < n_by_name; i++)
        if (strcmp(by_name[i].name, name) == 0)
            return 1;
    return 0;
}

/* by_name[name] = c.  A repeated HELLO leaves the previous entry in place,
 * which is the Python's behaviour and its leak; the table is bounded here, so
 * an overflow drops the oldest entry rather than growing without limit. */
static void name_set(const char *name, int ci)
{
    for (int i = 0; i < n_by_name; i++) {
        if (strcmp(by_name[i].name, name) == 0) {
            by_name[i].ci  = ci;
            by_name[i].gen = clients[ci].gen;
            return;
        }
    }
    if (n_by_name >= NAME_SLOTS) {
        memmove(&by_name[0], &by_name[1],
                (size_t)(NAME_SLOTS - 1) * sizeof by_name[0]);
        n_by_name--;
    }
    snprintf(by_name[n_by_name].name, sizeof by_name[0].name, "%s", name);
    by_name[n_by_name].ci  = ci;
    by_name[n_by_name].gen = clients[ci].gen;
    n_by_name++;
}

/* del by_name[c.name], but only `if self.by_name.get(c.name) is c`. */
static void name_del_if_is(const char *name, int ci)
{
    for (int i = 0; i < n_by_name; i++) {
        if (strcmp(by_name[i].name, name) == 0) {
            if (by_name[i].ci != ci || by_name[i].gen != clients[ci].gen)
                return;
            memmove(&by_name[i], &by_name[i + 1],
                    (size_t)(n_by_name - i - 1) * sizeof by_name[0]);
            n_by_name--;
            return;
        }
    }
}

/* ---- the per-client CRC map -------------------------------------------- */

static int crc_find(const client_t *c, uint64_t key)
{
    for (int i = 0; i < c->ncrc; i++)
        if (c->crc[i].key == key)
            return i;
    return -1;
}

/* c.crc[key] = val: an existing key KEEPS its position and takes the new
 * value; a new key is appended. */
static void crc_set(client_t *c, uint64_t key, uint8_t val)
{
    int i = crc_find(c, key);
    if (i >= 0) {
        c->crc[i].crc = val;
        return;
    }
    if (c->ncrc >= CRC_SLOTS)           /* unreachable: trimmed to 8 below */
        c->ncrc = CRC_SLOTS - 1;
    c->crc[c->ncrc].key = key;
    c->crc[c->ncrc].crc = val;
    c->ncrc++;
}

static void crc_del_at(client_t *c, int i)
{
    memmove(&c->crc[i], &c->crc[i + 1],
            (size_t)(c->ncrc - i - 1) * sizeof c->crc[0]);
    c->ncrc--;
}

/* ---- plumbing ---------------------------------------------------------- */

static void lobby_update_count(void)
{
    if (lob)
        lobby_update(lob, n_by_name);
}

/* reselect(): EVENT_READ | (EVENT_WRITE if c.tx else 0) */
static void update_events(int ci)
{
    client_t *c = &clients[ci];
    uint32_t want = EPOLLIN | (c->tx_len ? (uint32_t)EPOLLOUT : 0u);
    struct epoll_event ev;

    if (c->dead || c->fd < 0 || want == c->armed)
        return;
    ev.events   = want;
    ev.data.u64 = tag_of(ci);
    if (epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev) == 0)
        c->armed = want;
}

/* Client.send(data): APPEND ONLY.  The Python never flushes here, and the
 * backlog is enforced by flush() alone -- see TX_CAP. */
static void client_send(int ci, const uint8_t *data, size_t n)
{
    client_t *c = &clients[ci];

    /* Unreachable in practice -- see TX_CAP, which carries a whole recv()
     * worth of headroom above the backlog cap.  Saturating rather than
     * copying guarantees the next flush() sees tx_len > MAX_TX_BACKLOG and
     * drops the client, which is where the Python ends up too. */
    if ((size_t)c->tx_len + n > TX_CAP) {
        c->tx_len = TX_CAP;
        return;
    }
    memcpy(txbuf[ci] + c->tx_len, data, n);
    c->tx_len += (uint32_t)n;
}

static void send_frame(int ci, uint8_t ftype, const uint8_t *payload, size_t plen)
{
    uint8_t buf[FRAME_MAX];
    size_t n = frame_build(buf, ftype, payload, plen);
    client_send(ci, buf, n);
}

/* def flush(self, c): ONE send(), then the backlog check on what is LEFT. */
static void flush(int ci)
{
    client_t *c = &clients[ci];
    ssize_t k;

    if (!c->tx_len)
        return;
    if (c->fd < 0) {
        drop(ci);                       /* send() on a closed socket: OSError */
        return;
    }
    for (;;) {
        k = send(c->fd, txbuf[ci], c->tx_len, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR)
            continue;                   /* PEP 475: Python retries too */
        break;
    }
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        /* except BlockingIOError: pass -- nothing was sent, fall through to
         * the backlog check exactly as the Python does. */
    } else if (k < 0) {
        drop(ci);                       /* except OSError: self.drop(c) */
        return;
    } else {
        memmove(txbuf[ci], txbuf[ci] + k,                   /* del tx[:sent] */
                c->tx_len - (size_t)k);
        c->tx_len -= (uint32_t)k;
    }
    if (c->tx_len > MAX_TX_BACKLOG) {
        stats.tx_drops++;
        LOG("%s: %u bytes backed up, dropping", name_or_q(ci),
            (unsigned)c->tx_len);
        drop(ci);
    }
}

static int client_alloc(int fd, const struct sockaddr_in *sa)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &clients[i];

        if (c->in_use)
            continue;
        /* Zeroed field by field rather than with memset, because gen has to
         * survive and because rxbuf[i]/txbuf[i] must NOT be touched: rx_len
         * and tx_len at zero are what make them empty, and leaving the pages
         * unfaulted is the whole point of splitting them out.  Every field of
         * client_t is accounted for below. */
        c->in_use  = 1;
        c->fd      = fd;
        c->rx_len  = 0;
        c->tx_len  = 0;
        c->name[0] = '\0';
        c->has_name = 0;
        c->tv      = -1;
        c->partner = -1;
        c->role    = -1;
        c->dead    = 0;
        c->armed   = 0;
        c->ncrc    = 0;             /* makes crc[] empty; contents irrelevant */
        c->have_last_tick = 0;
        c->last_tick = 0;
        c->lap     = 0;
        c->ip      = sa->sin_addr.s_addr;
        c->born    = mono();
        inet_ntop(AF_INET, &sa->sin_addr, c->ipstr, sizeof c->ipstr);
        c->peer_port = ntohs(sa->sin_port);
        order_add(i);
        return i;
    }
    return -1;
}

static void client_free(int ci)
{
    clients[ci].in_use = 0;
    clients[ci].gen++;                  /* stale epoll tags stop matching */
}

static void accept_client(int srv)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    char ipbuf[INET_ADDRSTRLEN];
    int per_ip = 0, ci, one = 1, fd;

    fd = accept(srv, (struct sockaddr *)&sa, &sl);
    if (fd < 0)
        return;

    if (n_order >= MAX_CLIENTS) {       /* silently, as the Python does */
        close(fd);
        return;
    }
    for (int i = 0; i < n_order; i++)
        if (clients[order[i]].ip == sa.sin_addr.s_addr)
            per_ip++;
    if (per_ip >= MAX_CONNECTIONS_PER_IP) {
        stats.ip_rejects++;
        close(fd);
        return;
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    /* Without this the relay's own forwarding is subject to Nagle, and the
     * delay lands squarely on the lockstep critical path. */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    ci = client_alloc(fd, &sa);
    if (ci < 0) {                       /* unreachable: n_order checked */
        close(fd);
        return;
    }
    {
        struct epoll_event ev = { .events = EPOLLIN,
                                  .data = { .u64 = tag_of(ci) } };
        if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) != 0) {
            close(fd);
            order_del(ci);
            client_free(ci);
            return;
        }
    }
    clients[ci].armed = EPOLLIN;
    inet_ntop(AF_INET, &sa.sin_addr, ipbuf, sizeof ipbuf);
    LOG("%s:%u connected", ipbuf, (unsigned)ntohs(sa.sin_port));
}

/* ---- framing ----------------------------------------------------------- */

static void service(int ci)
{
    client_t *c = &clients[ci];
    uint8_t buf[4096];
    ssize_t n;

    for (;;) {
        n = recv(c->fd, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    /* `except OSError: data = b""` -- BlockingIOError is an OSError subclass,
     * so the Python drops on EAGAIN too.  Level-triggered epoll makes that
     * unreachable in both implementations; matching it costs nothing. */
    if (n <= 0) {
        drop(ci);
        return;
    }
    /* Unreachable, and stands as a production assert.  The framing loop below
     * only ever returns with a PARTIAL frame in rx, and a partial frame is at
     * most 200 bytes (lenb <= 200), so rx_len <= 200 on entry and 200 + 4096
     * always fits in RX_CAP.  The Python's rx is an unbounded bytearray and so
     * has no equivalent line; if this ever fires it is a bug here, not a
     * divergence, hence the distinct wording. */
    if (c->rx_len + (size_t)n > RX_CAP) {
        LOG("%s: rx overflow (%u + %zd > %d), dropping the connection",
            name_or_q(ci), (unsigned)c->rx_len, n, RX_CAP);
        drop(ci);
        return;
    }
    memcpy(rxbuf[ci] + c->rx_len, buf, (size_t)n);
    c->rx_len += (uint32_t)n;

    /* The Python's frame loop has NO liveness check after handle(): it keeps
     * parsing the rest of rx even once the client has been dropped, and
     * drop() clears only the PARTNER's link, so a dropped console can still
     * forward a queued frame to its ex-partner.  The slot stays allocated
     * until the end of this event-loop iteration precisely so that this
     * stays observable here instead of being a use-after-free. */
    for (;;) {
        unsigned lenb, need;
        uint8_t body[201];

        if (c->rx_len < 1)
            return;
        lenb = rxbuf[ci][0];
        /* Structural validation at the door.  A length of 0 cannot be a frame
         * (it would carry no type) and nothing this protocol defines is
         * anywhere near 200 bytes. */
        if (lenb < 1 || lenb > 200) {
            stats.bad_frames++;
            LOG("%s: bad frame length %u, dropping the connection",
                name_or_q(ci), lenb);
            drop(ci);
            return;
        }
        need = lenb + 1u;
        if (c->rx_len < need)
            return;                     /* wait for the rest of the frame */
        memcpy(body, rxbuf[ci] + 1, lenb);
        memmove(rxbuf[ci], rxbuf[ci] + need, c->rx_len - need);
        c->rx_len -= need;
        handle(ci, body[0], body + 1, lenb - 1u);
    }
}

/* ---- the lobby --------------------------------------------------------- */

static void log_crc(int ci, const uint8_t *payload);
static void on_hello(int ci, const uint8_t *payload, size_t plen);
static void try_pair(int ci);
static void send_lobby(int ci);

static void handle(int ci, uint8_t ftype, const uint8_t *payload, size_t plen)
{
    client_t *c = &clients[ci];

    if (IS_RELAY_TYPE(ftype)) {
        if (c->partner >= 0) {
            /* Padded on the way out, never on the way in: a console may send
             * a short record, but one must never RECEIVE a short one.  ljust
             * PADS BUT NEVER TRUNCATES -- an over-long payload is forwarded
             * at its own length. */
            uint8_t out[FRAME_MAX];
            size_t outlen = plen < GAME_PAYLOAD ? (size_t)GAME_PAYLOAD : plen;
            if (plen)
                memcpy(out, payload, plen);
            if (outlen > plen)
                memset(out + plen, 0, outlen - plen);
            send_frame(c->partner, ftype, out, outlen);
        }
        if (ftype == T_INPUT && plen >= CRC_MIN_PAYLOAD)
            log_crc(ci, payload);
        return;
    }

    if (ftype == T_HELLO) {
        on_hello(ci, payload, plen);
    } else if (ftype == T_LIST) {
        send_lobby(ci);
    } else if (ftype == T_JOIN) {
        /* Reserved.  The ROM auto-pairs today; keeping JOIN in the protocol
         * is what lets an in-ROM lobby list arrive later without a wire
         * change. */
        try_pair(ci);
    } else if (ftype == T_PING) {
        send_frame(ci, T_PONG, NULL, 0);
    } else if (ftype == T_BYE) {
        drop(ci);
    } else {
        LOG("%s: unknown frame type $%02X", name_or_q(ci), ftype);
    }
}

static void on_hello(int ci, const uint8_t *payload, size_t plen)
{
    client_t *c = &clients[ci];
    uint8_t up[256];
    const uint8_t *nm;
    size_t nlen;
    char name[9];
    const char *tvname;
    int guard = 0;

    if (plen < 3 || payload[0] != PROTO) {
        char ver[8];
        if (plen >= 1)
            snprintf(ver, sizeof ver, "%02x", payload[0]);
        else
            snprintf(ver, sizeof ver, "none");
        LOG("%s: HELLO version %s, wanted %d", c->ipstr, ver, PROTO);
        drop(ci);
        return;
    }

    /* THE TELEVISION STANDARD, AND THE ONE THIS SERVER WILL NOT PAIR.
     *
     * A 2600 cannot measure which television it is plugged into -- the ROM
     * generates the video timing and there is nothing to read -- so the
     * console declares what it was BUILT as and build.sh will not build a
     * SECAM image.  This is the second lock, for a hand-built one: on a SECAM
     * TIA the eight colours are chosen by the hue nibble and the luminance
     * bits are ignored, so Combat's two tanks, which differ only in colour,
     * can render identically.  It is not a sync problem and lockstep does not
     * help it, so the console is refused rather than guarded. */
    c->tv = payload[1];
    if (c->tv == TV_SECAM) {
        LOG("%s: refusing a SECAM console -- its TIA cannot tell the two "
            "tanks apart by colour", c->ipstr);
        drop(ci);
        return;
    }
    if (c->tv != TV_NTSC && c->tv != TV_PAL) {
        LOG("%s: HELLO declares television standard $%02X, which is not "
            "one this server knows", c->ipstr, (unsigned)c->tv);
        drop(ci);
        return;
    }

    /* payload[2:].decode("ascii","replace").strip("\x00 ").upper().
     *
     * Every byte >= 0x80 decodes to exactly ONE U+FFFD, so character count
     * equals byte count and the charset test rejects it either way: working
     * on bytes yields an identical accept/reject decision on every input. */
    nm   = payload + 2;
    nlen = plen - 2;
    while (nlen && (nm[0] == 0x00 || nm[0] == ' ')) { nm++; nlen--; }
    while (nlen && (nm[nlen - 1] == 0x00 || nm[nlen - 1] == ' ')) nlen--;
    if (nlen > sizeof up)
        nlen = sizeof up;
    for (size_t i = 0; i < nlen; i++)
        up[i] = (nm[i] >= 'a' && nm[i] <= 'z') ? (uint8_t)(nm[i] - 32) : nm[i];

    {
        int ok = (nlen >= 2 && nlen <= 8);
        for (size_t i = 0; ok && i < nlen; i++)
            if (!((up[i] >= 'A' && up[i] <= 'Z') ||
                  (up[i] >= '0' && up[i] <= '9')))
                ok = 0;
        if (!ok) {
            LOG("%s: bad name %s", c->ipstr, py_repr(up, nlen));
            drop(ci);
            return;
        }
    }

    memcpy(name, up, nlen);
    name[nlen] = '\0';
    /* while name in self.by_name: name = name[:7] + str(randrange(10)).  The
     * Python's loop is unbounded; bound it, and refuse rather than spin. */
    while (name_taken(name)) {
        size_t l = strlen(name);
        if (++guard > 1000) {
            LOG("%s: cannot find a free name for %s", c->ipstr, name);
            drop(ci);
            return;
        }
        if (l > 7)
            l = 7;
        name[l]     = (char)('0' + next_digit());
        name[l + 1] = '\0';
    }

    snprintf(c->name, sizeof c->name, "%s", name);
    c->has_name = 1;
    name_set(name, ci);
    tvname = (c->tv == TV_NTSC) ? "NTSC" : "PAL";
    LOG("%s is %s (%s)", c->ipstr, name, tvname);
    lobby_update_count();
    try_pair(ci);
}

/* Auto-pair: the first two idle consoles become a match.
 *
 * The ROM does not send JOIN.  A 2600 client has twelve columns of
 * cartridge-composed text and no keyboard, so an in-ROM lobby list costs a
 * whole bank; the Lobby already does room discovery, and what is left for
 * this server is to put the two consoles that turned up together. */
static void try_pair(int ci)
{
    int other = -1, host, guest;
    uint16_t seed;

    /* Oldest waiting first, so a console cannot be queue-jumped forever.
     * min() keeps the FIRST minimum, and self.clients is accept-ordered. */
    for (int i = 0; i < n_order; i++) {
        int o = order[i];
        if (o == ci || !clients[o].has_name || clients[o].partner >= 0)
            continue;
        if (other < 0 || clients[o].born < clients[other].born)
            other = o;
    }
    if (other < 0)
        return;

    seed = next_seed();
    /* The console that was already waiting is the host and drives player 0 --
     * the same convention as the Intellivision family, where the waiting
     * player takes role 0.  Player 0 is the LEFT port's paddle A, and the
     * guest's is player 2, the right port's: the game itself says so at $F206,
     * which computes the second live player as the first EOR 2. */
    host  = other;
    guest = ci;
    clients[host].partner  = guest;
    clients[guest].partner = host;
    clients[host].role  = 0;
    clients[guest].role = 1;

    for (int k = 0; k < 2; k++) {
        int me   = k ? guest : host;
        int them = k ? host  : guest;
        uint8_t pl[13];

        clients[me].ncrc = 0;           /* me.crc.clear() */
        pl[0] = (uint8_t)clients[me].role;
        pl[1] = (uint8_t)(seed & 0xFF);
        pl[2] = (uint8_t)(seed >> 8);
        pl[3] = (uint8_t)g_delay;
        pl[4] = (uint8_t)g_variation;
        pad_name(pl + 5, clients[them].name);
        send_frame(me, T_START, pl, sizeof pl);
    }
    stats.matches++;
    LOG("match: %s (host) vs %s, seed $%04X, delay %d, variation %d",
        clients[host].name, clients[guest].name, (unsigned)seed,
        g_delay, g_variation);
    lobby_update_count();
}

static void send_lobby(int ci)
{
    uint8_t pl[1 + 8 * 9];
    int n = 0;

    for (int i = 0; i < n_order; i++) {
        int o = order[i];
        if (!clients[o].has_name || n >= 8)
            continue;
        pad_name(pl + 1 + n * 9, clients[o].name);
        pl[1 + n * 9 + 8] = clients[o].partner >= 0 ? 1 : 0;
        n++;
    }
    pl[0] = (uint8_t)n;
    send_frame(ci, T_LOBBY, pl, 1 + (size_t)n * 9);
}

/* ---- the CRCs ----------------------------------------------------------- */

/* Pair the two consoles' checksums by tick and report disagreement.
 *
 * The relay does not act on a mismatch -- repair is the ROM's job, and a
 * server that tried would be guessing at a simulation it cannot see.  What it
 * can do is say, in one line, whether the two consoles ever disagreed, which
 * is the whole verdict test/run_rig.sh reads.
 *
 * The wire tick is EIGHT BITS and wraps every 256 ticks.  The console does not
 * care -- every comparison it makes is signed -- but the relay pairs checksums
 * BY tick, and lap 2's tick 5 is a different tick from lap 1's, so the lap is
 * reconstructed from the stream itself: a tick that jumps backwards by more
 * than half the range is a wrap, not a reorder. */
static void log_crc(int ci, const uint8_t *payload)
{
    client_t *c = &clients[ci];
    uint8_t tick = payload[IR_TICK], crc = payload[IR_CRC];
    uint64_t key;
    int p, pi;

    if (c->have_last_tick) {
        uint8_t delta = (uint8_t)(tick - c->last_tick);
        if (delta > 0 && delta < 128 && tick < c->last_tick)
            c->lap++;
    }
    c->have_last_tick = 1;
    c->last_tick = tick;
    key = c->lap * 256u + tick;

    crc_set(c, key, crc);
    p  = c->partner;
    pi = (p >= 0) ? crc_find(&clients[p], key) : -1;
    if (pi < 0) {
        /* Evict by AGE, never by value.  A dict preserves insertion order, so
         * the oldest is simply first.  Eight is about half a second: both
         * consoles send every tick, so an entry that has not paired within a
         * handful of them is one whose partner never sent it. */
        while (c->ncrc > 8)
            crc_del_at(c, 0);
        return;
    }
    {
        uint8_t other = clients[p].crc[pi].crc;
        crc_del_at(&clients[p], pi);
        if (other != crc) {
            stats.crc_bad++;
            LOG("CRC MISMATCH tick %d: %s=$%02X vs $%02X",
                (int)tick, c->name, (unsigned)crc, (unsigned)other);
        } else {
            stats.crc_ok++;
        }
    }
}

/* ---- drops -------------------------------------------------------------- */

static void drop(int ci)
{
    client_t *c = &clients[ci];
    int p;

    if (c->dead)                        /* if c.sock not in self.clients */
        return;
    c->dead = 1;

    if (c->has_name)
        name_del_if_is(c->name, ci);

    /* Note what is NOT done here: c.partner is left pointing at the survivor,
     * which is what lets a dropped console's remaining queued frames still be
     * forwarded.  Only the survivor's link back is cleared. */
    p = c->partner;
    if (p >= 0) {
        uint8_t zero[GAME_PAYLOAD] = { 0 };
        clients[p].partner = -1;
        send_frame(p, T_PEER_LEFT, zero, sizeof zero);
        LOG("match ended: %s left, %u CRC rounds verified",
            name_or_q(ci), stats.crc_ok);
    }

    if (c->fd >= 0) {
        epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
        c->fd = -1;
    }
    order_del(ci);

    if (c->has_name)
        LOG("%s disconnected", c->name);
    lobby_update_count();
}

/* Free every slot drop() retired.  Deferred to the end of the event-loop
 * iteration so a slot outlives the service() loop that killed it, the way a
 * Python object outlives its removal from self.clients. */
static void reap(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].in_use && clients[i].dead)
            client_free(i);
}

static void sweep(double now)
{
    int snap[MAX_CLIENTS], n = n_order;

    memcpy(snap, order, (size_t)n * sizeof snap[0]);
    for (int i = 0; i < n; i++) {
        client_t *c = &clients[snap[i]];
        if (c->in_use && !c->dead && !c->has_name &&
            now - c->born > HELLO_TIMEOUT) {
            stats.hello_timeouts++;
            drop(snap[i]);
        }
    }
}

static void log_stats(void)
{
    /* log("stats %s", self.stats) -- a Python dict repr, in INSERTION order.
     * Not sorted: the Intellivision port sorts because its Python does. */
    LOG("stats {'matches': %u, 'crc_ok': %u, 'crc_bad': %u, "
        "'bad_frames': %u, 'tx_drops': %u, 'hello_timeouts': %u, "
        "'ip_rejects': %u}",
        stats.matches, stats.crc_ok, stats.crc_bad, stats.bad_frames,
        stats.tx_drops, stats.hello_timeouts, stats.ip_rejects);
}

/* ---- signals ------------------------------------------------------------ */

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
    if (sigpipe_fd[1] >= 0) {
        ssize_t r = write(sigpipe_fd[1], "x", 1);
        (void)r;
    }
}

/* ---- main --------------------------------------------------------------- */

static void usage(FILE *f)
{
    fprintf(f,
"usage: vo-relay [options]\n"
"  --host ADDR              bind address (default 0.0.0.0)\n"
"  --port N                 listen port (default 9600)\n"
"  --delay N                input delay in ticks (default %d; measured at\n"
"                           3 frames a tick)\n"
"  --variation N            variation the match starts on, %d-%d ($96's own\n"
"                           numbering: manual game N is N-1). Must be one a\n"
"                           pair of remote players can actually play\n"
"  --any-variation          allow a start variation outside the two-player set\n"
"  --lobby-url URL          e.g. http://lobby.fujinet.online:8080; off by\n"
"                           default.  http:// only -- this build has no TLS\n"
"  --lobby-appkey N         FujiNet-registry appkey id (default 24)\n"
"  --lobby-serverurl URL    public endpoint clients should use\n"
"  --lobby-client-url URL   TNFS path of the client ROM for Lobby boot\n"
"  --game-name S            game name for the Lobby registration\n"
"  --server-name S          server name for the Lobby registration\n"
"  --region S               ISO region, lowercase (default us)\n"
"  --lobby-keepalive SECS   re-POST interval (default 300; test hook)\n"
"  --fixed-seed N           test only: pin the match seed to N (1..65535)\n",
        DEFAULT_DELAY, VARIATION_MIN, VARIATION_MAX);
}

int main(int argc, char **argv)
{
    const char *lobby_url    = "";
    const char *lobby_srvurl = "TCP://fujinet.online:9600/";
    const char *lobby_cliurl =
        "TNFS://apps.irata.online/Atari2600/Games/VideoOlympics.bin";
    const char *region       = "us";
    const char *game_name    = "Video Olympics";
    const char *server_name  = "Video Olympics Netplay";
    int    lobby_appkey    = 24;
    double lobby_keepalive = 300.0;
    int    srv, one = 1;
    struct sockaddr_in sa;
    double last_sweep, last_stats_at;
    static char logbuf[8192];

    setvbuf(stdout, logbuf, _IOLBF, sizeof logbuf);
    tzset();

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *eq = strchr(a, '=');
        char flag[32];
        const char *val = NULL;

        if (strncmp(a, "--", 2) != 0) {
            fprintf(stderr, "unexpected argument: %s\n", a);
            return 2;
        }
        if (eq) {
            size_t fl = (size_t)(eq - a);
            if (fl >= sizeof flag) {
                fprintf(stderr, "bad flag: %s\n", a);
                return 2;
            }
            memcpy(flag, a, fl);
            flag[fl] = '\0';
            val = eq + 1;
        } else {
            snprintf(flag, sizeof flag, "%s", a);
        }

#define NEEDVAL()                                                             \
        do {                                                                  \
            if (!val) {                                                       \
                if (i + 1 >= argc) {                                          \
                    fprintf(stderr, "%s requires a value\n", flag);           \
                    return 2;                                                 \
                }                                                             \
                val = argv[++i];                                              \
            }                                                                 \
        } while (0)

        if (!strcmp(flag, "--help") || !strcmp(flag, "-h")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(flag, "--host")) {
            NEEDVAL(); g_host = val;
        } else if (!strcmp(flag, "--port")) {
            NEEDVAL(); g_port = atoi(val);
        } else if (!strcmp(flag, "--delay")) {
            NEEDVAL(); g_delay = atoi(val);
        } else if (!strcmp(flag, "--variation")) {
            NEEDVAL(); g_variation = atoi(val);
        } else if (!strcmp(flag, "--any-variation")) {
            g_any_variation = 1;
        } else if (!strcmp(flag, "--lobby-url")) {
            NEEDVAL(); lobby_url = val;
        } else if (!strcmp(flag, "--lobby-appkey")) {
            NEEDVAL(); lobby_appkey = atoi(val);
        } else if (!strcmp(flag, "--lobby-serverurl")) {
            NEEDVAL(); lobby_srvurl = val;
        } else if (!strcmp(flag, "--lobby-client-url")) {
            NEEDVAL(); lobby_cliurl = val;
        } else if (!strcmp(flag, "--game-name")) {
            NEEDVAL(); game_name = val;
        } else if (!strcmp(flag, "--server-name")) {
            NEEDVAL(); server_name = val;
        } else if (!strcmp(flag, "--region")) {
            NEEDVAL(); region = val;
        } else if (!strcmp(flag, "--lobby-keepalive")) {
            NEEDVAL(); lobby_keepalive = atof(val);
        } else if (!strcmp(flag, "--fixed-seed")) {
            NEEDVAL(); g_fixed_seed = atoi(val);
        } else {
            fprintf(stderr, "unknown flag: %s\n", flag);
            usage(stderr);
            return 2;
        }
#undef NEEDVAL
    }

    if (g_variation < VARIATION_MIN || g_variation > VARIATION_MAX) {
        fprintf(stderr, "vo_relay: variation must be %d-%d\n",
                VARIATION_MIN, VARIATION_MAX);
        return 1;
    }
    if (!is_two_player(g_variation) && !g_any_variation) {
        fprintf(stderr, "vo_relay: variation %d (manual game %d) is not a "
                "two-player game. Two of the fifty are single-player and "
                "twenty-six need four paddles. Pass --any-variation to "
                "override.\n", g_variation, g_variation + 1);
        return 1;
    }
    if (g_port < 1 || g_port > 65535) {
        fprintf(stderr, "vo_relay: port must be 1..65535\n");
        return 2;
    }
    if (g_fixed_seed < 0 || g_fixed_seed > 0xFFFF) {
        fprintf(stderr, "vo_relay: --fixed-seed must be 1..65535\n");
        return 2;
    }

    {   /* seed the PRNG */
        uint64_t s = 0;
        if (getrandom(&s, sizeof s, 0) != (ssize_t)sizeof s)
            s = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
        rng_state = s;
    }

    /* Keep the transmit and receive buffers off huge pages.  A steady-state
     * client only ever writes into the first page of its 256 KiB transmit
     * buffer; on a 2 MiB huge page that first write would make 2 MiB resident
     * and hold it.  MADV_NOHUGEPAGE is advisory and the relay works without
     * it, so the return value is deliberately unchecked. */
    (void)madvise(txbuf, sizeof txbuf, MADV_NOHUGEPAGE);
    (void)madvise(rxbuf, sizeof rxbuf, MADV_NOHUGEPAGE);

    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;

    signal(SIGPIPE, SIG_IGN);   /* CPython does this at startup; MSG_NOSIGNAL
                                 * covers send(), this covers everything else */

    if (pipe(sigpipe_fd) != 0) {
        perror("pipe");
        return 1;
    }
    fcntl(sigpipe_fd[0], F_SETFL, O_NONBLOCK);
    fcntl(sigpipe_fd[1], F_SETFL, O_NONBLOCK);
    {
        /* SIGTERM, not just SIGINT.  The Lobby has no expiry job, so a room
         * whose relay died without publishing "offline" sits in every
         * client's list for ever. */
        struct sigaction act;
        memset(&act, 0, sizeof act);
        act.sa_handler = on_signal;
        sigemptyset(&act.sa_mask);
        act.sa_flags = SA_RESTART;
        sigaction(SIGINT, &act, NULL);
        sigaction(SIGTERM, &act, NULL);
    }

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)g_port);
    if (inet_pton(AF_INET, g_host, &sa.sin_addr) != 1) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(g_host, NULL, &hints, &res) != 0 || !res) {
            fprintf(stderr, "vo_relay: cannot resolve --host %s\n", g_host);
            return 1;
        }
        sa.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(srv, 8) != 0) {
        perror("listen");
        return 1;
    }
    fcntl(srv, F_SETFL, fcntl(srv, F_GETFL, 0) | O_NONBLOCK);

    ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) {
        perror("epoll_create1");
        return 1;
    }
    {
        struct epoll_event ev = { .events = EPOLLIN,
                                  .data = { .u64 = TAG_LISTEN } };
        epoll_ctl(ep, EPOLL_CTL_ADD, srv, &ev);
        ev.data.u64 = TAG_SIG;
        epoll_ctl(ep, EPOLL_CTL_ADD, sigpipe_fd[0], &ev);
    }

    LOG("listening on %s:%d, delay %d ticks", g_host, g_port, g_delay);

    if (lobby_url[0]) {
        lob = lobby_start(lobby_url, lobby_srvurl, lobby_cliurl, lobby_appkey,
                          region, game_name, server_name, 2, lobby_keepalive);
    }

    last_sweep = last_stats_at = mono();
    while (!stop_requested) {
        struct epoll_event evs[MAX_CLIENTS + 2];
        int snap[MAX_CLIENTS], ns;
        double now;
        int n = epoll_wait(ep, evs, (int)(sizeof evs / sizeof evs[0]), 1000);

        if (n < 0 && errno == EINTR)
            n = 0;
        if (n < 0) {
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            uint64_t t = evs[i].data.u64;
            uint32_t m = evs[i].events;
            int ci;

            if (t == TAG_LISTEN) {
                accept_client(srv);     /* one accept per readiness event,
                                         * level-triggered: exactly what
                                         * selectors does */
                continue;
            }
            if (t == TAG_SIG) {
                char sink[64];
                while (read(sigpipe_fd[0], sink, sizeof sink) > 0)
                    ;
                continue;
            }
            ci = (int)(t & 0xFFFFFFFFu);
            if (ci < 0 || ci >= MAX_CLIENTS)
                continue;
            /* A client can be dropped and its slot reused by an accept()
             * inside this very batch.  Python is immune by object identity;
             * the generation half of the tag is what makes C immune. */
            if (!clients[ci].in_use || clients[ci].gen != (uint32_t)(t >> 32))
                continue;
            if (clients[ci].dead)
                continue;
            /* READ first, then WRITE -- the order the Python's loop uses. */
            if (m & (EPOLLIN | EPOLLHUP | EPOLLERR))
                service(ci);
            /* `if c.sock in self.clients and mask & EVENT_WRITE` */
            if (!clients[ci].dead && (m & EPOLLOUT))
                flush(ci);
        }

        /* for c in list(self.clients.values()): self.flush(c); self.reselect(c) */
        ns = n_order;
        memcpy(snap, order, (size_t)ns * sizeof snap[0]);
        for (int i = 0; i < ns; i++) {
            int ci = snap[i];
            if (!clients[ci].in_use || clients[ci].dead)
                continue;
            flush(ci);
            if (!clients[ci].dead)
                update_events(ci);
        }

        now = mono();
        if (now - last_sweep > SWEEP_INTERVAL) {
            sweep(now);
            last_sweep = now;
        }
        if (now - last_stats_at > STATS_INTERVAL) {
            log_stats();
            last_stats_at = now;
        }
        reap();
    }

    LOG("shutting down");
    if (lob)
        lobby_shutdown(lob);
    close(srv);
    close(ep);
    return 0;
}
