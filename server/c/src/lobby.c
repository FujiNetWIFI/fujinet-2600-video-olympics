/* FujiNet Lobby publisher.  See lobby.h and the LobbyPublisher docstring in
 * ../../combat_relay_server.py, which this file transliterates.
 *
 * Transport is a hand-rolled plain-HTTP POST over a raw socket: the Lobby's Go
 * server binds :8080 and the relay reaches it directly, so there is no TLS to
 * terminate and no dependency to link.  The Python's example
 * https://lobby.fujinet.online/server is the TLS proxy in front of that same
 * service; --lobby-url takes an http:// URL here and is refused otherwise.
 *
 * --lobby-url is the COMPLETE endpoint, path included -- the Python hands it
 * to urllib verbatim and so does this.  (The Intellivision relay appends
 * "/server" to a base URL; that is not this family's convention.)
 *
 * The emitted JSON is byte-identical to what json.dumps() produces for the
 * Python payload dict: same key order, ", " and ": " separators, one line.
 *
 * The platform string must be "a2600" -- that is what the 2600 Lobby client
 * queries (/view?bin=2&platform=a2600), and a row published under any other
 * spelling simply never appears in it.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "lobby.h"

#define HOST_MAX    256
#define PATH_MAX_   256
#define STR_MAX     256
#define JSON_MAX   4096
#define REQ_MAX    (JSON_MAX + 1024)
#define RESP_MAX   2048
#define HTTP_TIMEOUT_MS 10000       /* == urlopen(..., timeout=10) */

struct lobby {
    char     host[HOST_MAX];        /* from --lobby-url                */
    char     path[PATH_MAX_];       /* prefix + "/server"              */
    int      port;
    char     game[STR_MAX];
    char     server[STR_MAX];
    char     region[64];
    char     serverurl[STR_MAX];
    char     client_url[STR_MAX];
    int      appkey;
    int      maxplayers;
    int      curplayers;
    int      offline;               /* status: "online" -> "offline"   */
    int      dirty;
    int      stopping;
    int      stop_evt;
    unsigned failures;
    double   keepalive;
    pthread_mutex_t mu;
    pthread_cond_t  cv;             /* CLOCK_MONOTONIC                 */
    pthread_t       th;
};

/* ---- JSON ------------------------------------------------------------- */

/* json.dumps' ESCAPE_DCT: the five short forms, \u00xx for the rest of the
 * C0 range.  Non-ASCII never reaches here -- lobby_start() rejects it rather
 * than reproduce ensure_ascii's \uXXXX output for bytes we have no business
 * accepting on a command line. */
static int json_esc(const char *src, char *dst, size_t sz)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        char tmp[8];
        const char *rep = tmp;
        size_t n;
        switch (*p) {
        case '"':  rep = "\\\""; n = 2; break;
        case '\\': rep = "\\\\"; n = 2; break;
        case '\b': rep = "\\b";  n = 2; break;
        case '\f': rep = "\\f";  n = 2; break;
        case '\n': rep = "\\n";  n = 2; break;
        case '\r': rep = "\\r";  n = 2; break;
        case '\t': rep = "\\t";  n = 2; break;
        default:
            if (*p < 0x20) {
                n = (size_t)snprintf(tmp, sizeof tmp, "\\u%04x", *p);
            } else {
                tmp[0] = (char)*p;
                n = 1;
            }
            break;
        }
        if (o + n >= sz)
            return -1;
        memcpy(dst + o, rep, n);
        o += n;
    }
    if (o >= sz)
        return -1;
    dst[o] = '\0';
    return 0;
}

/* Caller holds l->mu. */
static int render_json(const struct lobby *l, char *buf, size_t sz)
{
    char g[STR_MAX * 6], s[STR_MAX * 6], r[64 * 6];
    char su[STR_MAX * 6], cu[STR_MAX * 6];

    if (json_esc(l->game, g, sizeof g) || json_esc(l->server, s, sizeof s) ||
        json_esc(l->region, r, sizeof r) ||
        json_esc(l->serverurl, su, sizeof su) ||
        json_esc(l->client_url, cu, sizeof cu))
        return -1;

    int n = snprintf(buf, sz,
        "{\"game\": \"%s\", \"appkey\": %d, \"server\": \"%s\", "
        "\"region\": \"%s\", \"serverurl\": \"%s\", \"status\": \"%s\", "
        "\"maxplayers\": %d, \"curplayers\": %d, "
        "\"clients\": [{\"platform\": \"a2600\", \"url\": \"%s\"}]}",
        g, l->appkey, s, r, su, l->offline ? "offline" : "online",
        l->maxplayers, l->curplayers, cu);
    return (n < 0 || (size_t)n >= sz) ? -1 : 0;
}

/* ---- HTTP ------------------------------------------------------------- */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* connect() with a deadline, so a black-holed Lobby cannot wedge the worker
 * (and through it, shutdown) for the kernel's default SYN timeout. */
static int dial(const char *host, int port, int64_t deadline)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;

    for (ai = res; ai; ai = ai->ai_next) {
        int flags, err = 0;
        socklen_t elen = sizeof err;
        struct pollfd pfd;
        int64_t left;

        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK,
                    ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        if (errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        left = deadline - now_ms();
        pfd.fd = fd;
        pfd.events = POLLOUT;
        if (left <= 0 || poll(&pfd, 1, (int)left) != 1 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err) {
            close(fd);
            fd = -1;
            continue;
        }
        flags = fcntl(fd, F_GETFL, 0);       /* back to blocking + SO_*TIMEO */
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        break;
    }
    freeaddrinfo(res);
    return fd;
}

/* POST the body and return the HTTP status code, or -1 on a transport error.
 * `err` receives a short reason for the log line. */
static int http_post(const struct lobby *l, const char *body,
                     char *err, size_t errsz)
{
    char req[REQ_MAX], resp[RESP_MAX];
    int64_t deadline = now_ms() + HTTP_TIMEOUT_MS;
    struct timeval tv;
    size_t blen = strlen(body), off = 0, got = 0;
    int fd, code = -1, rlen;

    rlen = snprintf(req, sizeof req,
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: combat-relay/1.0\r\n"
        "Accept: */*\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n%s",
        l->path, l->host, l->port, blen, body);
    if (rlen < 0 || (size_t)rlen >= sizeof req) {
        snprintf(err, errsz, "request too large");
        return -1;
    }

    fd = dial(l->host, l->port, deadline);
    if (fd < 0) {
        snprintf(err, errsz, "cannot connect to %s:%d", l->host, l->port);
        return -1;
    }

    tv.tv_sec  = HTTP_TIMEOUT_MS / 1000;
    tv.tv_usec = (HTTP_TIMEOUT_MS % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    while (off < (size_t)rlen) {
        ssize_t k = send(fd, req + off, (size_t)rlen - off, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0) {
            snprintf(err, errsz, "send failed: %s", strerror(errno));
            close(fd);
            return -1;
        }
        off += (size_t)k;
    }

    /* Connection: close means "read to EOF"; we only need the status line,
     * but draining keeps the peer from seeing a reset. */
    for (;;) {
        ssize_t k = read(fd, resp + got, sizeof resp - 1 - got);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            break;
        got += (size_t)k;
        if (got >= sizeof resp - 1)
            break;
    }
    close(fd);
    resp[got] = '\0';

    if (got == 0 || sscanf(resp, "HTTP/%*d.%*d %d", &code) != 1) {
        snprintf(err, errsz, "no HTTP response");
        return -1;
    }
    return code;
}

/* Returns 0 on success.  urllib.request.urlopen raises HTTPError -- an
 * OSError subclass -- for 4xx/5xx, so the Python counts those as failures
 * too; matching that is why the status check is here. */
static int do_post(const struct lobby *l, const char *body)
{
    char err[HOST_MAX + 64] = "";
    int code = http_post(l, body, err, sizeof err);

    if (code < 0) {
        relay_log("INFO", "lobby: POST failed (%s)", err);
        return -1;
    }
    if (code >= 400) {
        relay_log("INFO", "lobby: POST failed (HTTP Error %d)", code);
        return -1;
    }
    return 0;
}

/* ---- worker ----------------------------------------------------------- */

static void deadline_in(struct timespec *ts, double secs)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += (time_t)secs;
    ts->tv_nsec += (long)((secs - (double)(time_t)secs) * 1e9);
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static void *lobby_worker(void *arg)
{
    struct lobby *l = arg;
    char snapshot[JSON_MAX];

    for (;;) {
        struct timespec dl;
        int rendered;

        pthread_mutex_lock(&l->mu);
        deadline_in(&dl, l->keepalive);
        while (!l->dirty && !l->stopping) {
            if (pthread_cond_timedwait(&l->cv, &l->mu, &dl) == ETIMEDOUT)
                l->dirty = 1;               /* keepalive: re-POST as-is */
        }
        l->dirty = 0;
        if (l->stopping) {
            pthread_mutex_unlock(&l->mu);
            break;
        }
        rendered = render_json(l, snapshot, sizeof snapshot);
        pthread_mutex_unlock(&l->mu);

        if (rendered == 0 && do_post(l, snapshot) != 0) {
            pthread_mutex_lock(&l->mu);
            l->failures++;
            pthread_mutex_unlock(&l->mu);
        }

        /* == self._stop_evt.wait(1.0): debounce, returns early on shutdown */
        pthread_mutex_lock(&l->mu);
        if (!l->stop_evt) {
            struct timespec d1;
            deadline_in(&d1, 1.0);
            pthread_cond_timedwait(&l->cv, &l->mu, &d1);
        }
        pthread_mutex_unlock(&l->mu);
    }
    return NULL;
}

/* ---- public ----------------------------------------------------------- */

static int copy_ascii(char *dst, size_t sz, const char *src, const char *what)
{
    size_t n = strlen(src);
    if (n >= sz) {
        relay_log("WARNING", "lobby: %s too long (%zu bytes, max %zu)",
                  what, n, sz - 1);
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (*p > 0x7F) {
            relay_log("WARNING", "lobby: %s contains a non-ASCII byte", what);
            return -1;
        }
    }
    memcpy(dst, src, n + 1);
    return 0;
}

/* http://host[:port][/prefix] -> host, port, path = prefix + "/server".
 * The path assembly mirrors base_url.rstrip("/") + "/server". */
static int parse_url(struct lobby *l, const char *url)
{
    const char *p, *slash, *colon;
    char hostport[HOST_MAX];
    size_t n;

    if (strncmp(url, "https://", 8) == 0) {
        relay_log("WARNING", "lobby: --lobby-url must be http:// -- this "
                             "build talks to the Lobby's plain HTTP port "
                             "(default 8080), not the TLS proxy");
        return -1;
    }
    if (strncmp(url, "http://", 7) != 0) {
        relay_log("WARNING", "lobby: --lobby-url must start with http://");
        return -1;
    }
    p = url + 7;
    slash = strchr(p, '/');
    n = slash ? (size_t)(slash - p) : strlen(p);
    if (n == 0 || n >= sizeof hostport) {
        relay_log("WARNING", "lobby: --lobby-url has no usable host");
        return -1;
    }
    memcpy(hostport, p, n);
    hostport[n] = '\0';

    /* Only split on the last colon when everything after it is digits, so a
     * hostname is never mangled. */
    l->port = 80;
    colon = strrchr(hostport, ':');
    if (colon && colon[1]) {
        const char *d = colon + 1;
        while (*d >= '0' && *d <= '9')
            d++;
        if (*d == '\0') {
            l->port = atoi(colon + 1);
            *(char *)colon = '\0';
        }
    }
    if (l->port < 1 || l->port > 65535) {
        relay_log("WARNING", "lobby: --lobby-url port out of range");
        return -1;
    }
    if (copy_ascii(l->host, sizeof l->host, hostport, "--lobby-url host"))
        return -1;

    {   /* The path is used VERBATIM: --lobby-url is the whole endpoint,
         * e.g. http://lobby.fujinet.online:8080/server.  urllib POSTs to "/"
         * when a URL carries no path at all, so do the same. */
        const char *want = (slash && slash[0]) ? slash : "/";
        if ((size_t)snprintf(l->path, sizeof l->path, "%s", want)
            >= sizeof l->path) {
            relay_log("WARNING", "lobby: --lobby-url path too long");
            return -1;
        }
    }
    return 0;
}

lobby_t *lobby_start(const char *base_url, const char *serverurl,
                     const char *client_url, int appkey, const char *region,
                     const char *game_name, const char *server_name,
                     int maxplayers, double keepalive_secs)
{
    struct lobby *l = calloc(1, sizeof *l);
    pthread_condattr_t ca;

    if (!l)
        return NULL;

    if (parse_url(l, base_url) ||
        copy_ascii(l->serverurl, sizeof l->serverurl, serverurl, "--lobby-serverurl") ||
        copy_ascii(l->client_url, sizeof l->client_url, client_url, "--lobby-client-url") ||
        copy_ascii(l->game, sizeof l->game, game_name, "--game-name") ||
        copy_ascii(l->server, sizeof l->server, server_name, "--server-name") ||
        copy_ascii(l->region, sizeof l->region, region, "--region")) {
        free(l);
        return NULL;
    }

    l->appkey     = appkey;
    l->maxplayers = maxplayers;
    l->keepalive  = keepalive_secs;
    /* LobbyPublisher.__init__ sets dirty = True, so the room is registered as
     * soon as the thread runs rather than one keepalive later. */
    l->dirty      = 1;

    pthread_mutex_init(&l->mu, NULL);
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&l->cv, &ca);
    pthread_condattr_destroy(&ca);

    if (pthread_create(&l->th, NULL, lobby_worker, l) != 0) {
        relay_log("WARNING", "lobby: cannot spawn publisher thread");
        pthread_cond_destroy(&l->cv);
        pthread_mutex_destroy(&l->mu);
        free(l);
        return NULL;
    }
    return l;
}

void lobby_update(lobby_t *l, int curplayers)
{
    if (!l)
        return;
    pthread_mutex_lock(&l->mu);
    if (!l->stopping) {
        l->curplayers = curplayers;
        l->dirty = 1;
        pthread_cond_broadcast(&l->cv);
    }
    pthread_mutex_unlock(&l->mu);
}

unsigned lobby_failures(lobby_t *l)
{
    unsigned f;
    if (!l)
        return 0;
    pthread_mutex_lock(&l->mu);
    f = l->failures;
    pthread_mutex_unlock(&l->mu);
    return f;
}

void lobby_shutdown(lobby_t *l)
{
    char snapshot[JSON_MAX];
    struct timespec dl;
    int rendered;

    if (!l)
        return;

    pthread_mutex_lock(&l->mu);
    l->stopping = 1;
    l->stop_evt = 1;
    l->offline  = 1;
    rendered = render_json(l, snapshot, sizeof snapshot);
    pthread_cond_broadcast(&l->cv);          /* two distinct wait sites */
    pthread_mutex_unlock(&l->mu);

    /* join(timeout=12): let an in-flight POST finish first.  Unlike the
     * condvar, timedjoin wants a CLOCK_REALTIME deadline. */
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += 12;
    if (pthread_timedjoin_np(l->th, NULL, &dl) != 0) {
        pthread_detach(l->th);
        /* Intentionally leaked: the worker may still hold pointers into l. */
        if (rendered == 0)
            do_post(l, snapshot);
        relay_log("INFO", "lobby: published offline");
        return;
    }

    if (rendered == 0 && do_post(l, snapshot) != 0)
        l->failures++;
    relay_log("INFO", "lobby: published offline");
    pthread_cond_destroy(&l->cv);
    pthread_mutex_destroy(&l->mu);
    free(l);
}
