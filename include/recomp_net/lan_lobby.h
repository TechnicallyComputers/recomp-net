#ifndef RNET_LAN_LOBBY_H
#define RNET_LAN_LOBBY_H

#include "recomp_net/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Small same-machine LAN-room registry used by launcher integrations.
 *
 * The registry complements (rather than replaces) a remote lobby service:
 * hosts may publish to both, and launchers merge this row with their remote
 * list. Two local processes share the record, remain in the room UI, and only
 * receive launch parameters after the host marks the room started. The stored
 * endpoint is still a normal LAN address usable by another machine when the
 * row is distributed by the remote lobby service. */

#define RNET_LAN_LOBBY_NAME_MAX       96
#define RNET_LAN_LOBBY_GAME_MAX       64
#define RNET_LAN_LOBBY_VERSION_MAX    32
#define RNET_LAN_LOBBY_ENDPOINT_MAX   64
#define RNET_LAN_LOBBY_PLAYER_MAX     64
#define RNET_LAN_LOBBY_PASSWORD_MAX   64

typedef struct RNetLanLobby {
    char name[RNET_LAN_LOBBY_NAME_MAX];
    char game[RNET_LAN_LOBBY_GAME_MAX];
    char game_version[RNET_LAN_LOBBY_VERSION_MAX];
    char endpoint[RNET_LAN_LOBBY_ENDPOINT_MAX];
    char host_name[RNET_LAN_LOBBY_PLAYER_MAX];
    char joiner_name[RNET_LAN_LOBBY_PLAYER_MAX];
    char password[RNET_LAN_LOBBY_PASSWORD_MAX];
    int started;
    int host_slot;
} RNetLanLobby;

enum {
    RNET_LAN_LOBBY_OK = 0,
    RNET_LAN_LOBBY_ERR_IO = -1,
    RNET_LAN_LOBBY_ERR_FULL = -2,
    RNET_LAN_LOBBY_ERR_PASSWORD = -3,
    RNET_LAN_LOBBY_ERR_IDENTITY = -4
};

/* Publish or replace the single local room record at path. */
int rnet_lan_lobby_publish(const char *path, const RNetLanLobby *lobby);

/* Read a room. expected_game/version may be NULL or empty to accept any. */
int rnet_lan_lobby_read(const char *path, const char *expected_game,
                        const char *expected_version, RNetLanLobby *out);

/* Claim the guest slot after validating identity and password. */
int rnet_lan_lobby_join(const char *path, const char *expected_game,
                        const char *expected_version, const char *password,
                        const char *player_name, RNetLanLobby *out);

/* Host removes the room. Guest leave clears the guest slot and start flag. */
int rnet_lan_lobby_leave(const char *path, int is_host);

/* Host-controlled waiting-room operations. */
int rnet_lan_lobby_set_started(const char *path, int started);
int rnet_lan_lobby_set_host_slot(const char *path, int host_slot);

#ifdef __cplusplus
}
#endif

#endif /* RNET_LAN_LOBBY_H */
