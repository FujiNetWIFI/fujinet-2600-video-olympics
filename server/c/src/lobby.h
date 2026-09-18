/* FujiNet Lobby publisher -- the C rendering of LobbyPublisher in
 * ../../combat_relay_server.py.
 *
 * One worker thread owns all HTTP traffic: updates are coalesced under a
 * condition variable and only the newest player count is published, at most
 * once per second, so connection churn can never fan out into unbounded
 * threads or overlapping requests.
 */
#ifndef LOBBY_H
#define LOBBY_H

typedef struct lobby lobby_t;

/* Spawn the publisher thread.  base_url is an http:// URL and is the COMPLETE
 * endpoint, path included -- the Lobby's Go server binds :8080 and we reach it
 * directly, so there is no TLS here.
 * Every string is copied.  Returns NULL if the URL is unusable, or if a
 * parameter is too long or contains a non-ASCII byte (see lobby.c: we refuse
 * rather than reproduce json.dumps' ensure_ascii \uXXXX escaping).
 * keepalive_secs is 300.0 in production; the flag exists because a subprocess
 * cannot be monkeypatched the way tools/test_lobby_pub.py patches the Python
 * class attribute. */
lobby_t *lobby_start(const char *base_url, const char *serverurl,
                     const char *client_url, int appkey, const char *region,
                     const char *game_name, const char *server_name,
                     int maxplayers, double keepalive_secs);

/* Publish a new player count.  Coalescing means this is cheap enough to call
 * on every state change, exactly as the Python does. */
void lobby_update(lobby_t *l, int curplayers);

/* status:"offline", join the worker (12s), then POST from the calling thread.
 * Mirrors LobbyPublisher.shutdown().  Frees the handle. */
void lobby_shutdown(lobby_t *l);

/* Publish failures so far, for the stats line.  Safe from the main thread. */
unsigned lobby_failures(lobby_t *l);

/* Provided by combat-relay.c: "HH:MM:SS " timestamped, line-buffered STDOUT,
 * locked so the publisher thread cannot interleave with the event loop.  The
 * 2600 log has no level field; the argument is accepted and ignored so this
 * file stays a drop-in against the Intellivision original. */
void relay_log(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* LOBBY_H */
