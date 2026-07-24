#include "rnet_ice_internal.h"

#if defined(RNET_ENABLE_ICE)

#include <juice/juice.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION RNetIceMutex;
static void ice_mutex_init(RNetIceMutex *mutex) { InitializeCriticalSection(mutex); }
static void ice_mutex_destroy(RNetIceMutex *mutex) { DeleteCriticalSection(mutex); }
static void ice_mutex_lock(RNetIceMutex *mutex) { EnterCriticalSection(mutex); }
static void ice_mutex_unlock(RNetIceMutex *mutex) { LeaveCriticalSection(mutex); }
#else
#include <pthread.h>
typedef pthread_mutex_t RNetIceMutex;
static void ice_mutex_init(RNetIceMutex *mutex) { (void)pthread_mutex_init(mutex, NULL); }
static void ice_mutex_destroy(RNetIceMutex *mutex) { (void)pthread_mutex_destroy(mutex); }
static void ice_mutex_lock(RNetIceMutex *mutex) { (void)pthread_mutex_lock(mutex); }
static void ice_mutex_unlock(RNetIceMutex *mutex) { (void)pthread_mutex_unlock(mutex); }
#endif

#define RNET_ICE_RECV_QUEUE 64
#define RNET_ICE_RECV_MAX 2048
#define RNET_ICE_CAND_QUEUE 32
#define RNET_ICE_CAND_MAX 280

typedef struct RNetIceRecvSlot
{
    rnet_u8 data[RNET_ICE_RECV_MAX];
    size_t len;
} RNetIceRecvSlot;

typedef struct RNetIceCandSlot
{
    char sdp[RNET_ICE_CAND_MAX];
} RNetIceCandSlot;

struct RNetIceAgent
{
    juice_agent_t *agent;
    RNetIceMutex mutex;
    RNetIceState state;
    RNetIceSignalEmitFn emit;
    void *user;
    int gathering_done_posted;
    int remote_desc_set;
    RNetIceRecvSlot recv_q[RNET_ICE_RECV_QUEUE];
    unsigned recv_head;
    unsigned recv_tail;
    unsigned recv_count;
    RNetIceCandSlot cand_q[RNET_ICE_CAND_QUEUE];
    unsigned cand_head;
    unsigned cand_tail;
    unsigned cand_count;
    char stun_host[128];
    char turn_host[128];
    char turn_user[192];
    char turn_pass[128];
    char bind_address[64];
    juice_turn_server_t turn;
    int turn_count;
    int controlling;
    int gather_started;
    int gather_pending;
    int selected_logged;
    int force_relay; /* runtime and/or RNET_ICE_FORCE_TURN */
};

static RNetIceState map_juice_state(juice_state_t st)
{
    switch (st)
    {
    case JUICE_STATE_GATHERING:
        return RNET_ICE_STATE_GATHERING;
    case JUICE_STATE_CONNECTING:
        return RNET_ICE_STATE_CONNECTING;
    case JUICE_STATE_CONNECTED:
        return RNET_ICE_STATE_CONNECTED;
    case JUICE_STATE_COMPLETED:
        return RNET_ICE_STATE_COMPLETED;
    case JUICE_STATE_FAILED:
        return RNET_ICE_STATE_FAILED;
    default:
        return RNET_ICE_STATE_IDLE;
    }
}

static void queue_recv(RNetIceAgent *a, const char *data, size_t size)
{
    if ((data == NULL) || (size == 0) || (size > RNET_ICE_RECV_MAX))
    {
        return;
    }
    if (a->recv_count >= RNET_ICE_RECV_QUEUE)
    {
        a->recv_head = (a->recv_head + 1U) % RNET_ICE_RECV_QUEUE;
        a->recv_count--;
    }
    memcpy(a->recv_q[a->recv_tail].data, data, size);
    a->recv_q[a->recv_tail].len = size;
    a->recv_tail = (a->recv_tail + 1U) % RNET_ICE_RECV_QUEUE;
    a->recv_count++;
}

static void queue_cand(RNetIceAgent *a, const char *sdp)
{
    if ((sdp == NULL) || (sdp[0] == '\0'))
    {
        return;
    }
    if (a->cand_count >= RNET_ICE_CAND_QUEUE)
    {
        a->cand_head = (a->cand_head + 1U) % RNET_ICE_CAND_QUEUE;
        a->cand_count--;
    }
    snprintf(a->cand_q[a->cand_tail].sdp, sizeof(a->cand_q[a->cand_tail].sdp), "%s", sdp);
    a->cand_tail = (a->cand_tail + 1U) % RNET_ICE_CAND_QUEUE;
    a->cand_count++;
}

static void log_selected_pair(RNetIceAgent *a)
{
    char local_cand[512];
    char remote_cand[512];
    char local_addr[256];
    char remote_addr[256];
    if (a == NULL || a->agent == NULL || a->selected_logged)
        return;
    local_cand[0] = remote_cand[0] = local_addr[0] = remote_addr[0] = '\0';
    if (juice_get_selected_candidates(a->agent, local_cand, sizeof(local_cand),
                                      remote_cand, sizeof(remote_cand)) == 0) {
        fprintf(stderr,
                "rnet_ice: selected candidates\n  local:  %s\n  remote: %s\n",
                local_cand[0] ? local_cand : "(none)",
                remote_cand[0] ? remote_cand : "(none)");
        a->selected_logged = 1;
    }
    if (juice_get_selected_addresses(a->agent, local_addr, sizeof(local_addr),
                                     remote_addr, sizeof(remote_addr)) == 0) {
        fprintf(stderr, "rnet_ice: selected addresses local=%s remote=%s\n",
                local_addr[0] ? local_addr : "(none)",
                remote_addr[0] ? remote_addr : "(none)");
    }
}

static void on_state_changed(juice_agent_t *agent, juice_state_t state, void *user_ptr)
{
    RNetIceAgent *a = (RNetIceAgent *)user_ptr;
    RNetIceState mapped;
    (void)agent;
    mapped = map_juice_state(state);
    ice_mutex_lock(&a->mutex);
    a->state = mapped;
    ice_mutex_unlock(&a->mutex);
    if (mapped == RNET_ICE_STATE_CONNECTED || mapped == RNET_ICE_STATE_COMPLETED)
        log_selected_pair(a);
    else if (mapped == RNET_ICE_STATE_FAILED)
        fprintf(stderr, "rnet_ice: state=FAILED (check STUN/TURN / NAT path)\n");
}

static int ice_candidate_is_relay(const char *sdp)
{
    return sdp != NULL && strstr(sdp, " typ relay") != NULL;
}

static void on_candidate(juice_agent_t *agent, const char *sdp, void *user_ptr)
{
    RNetIceAgent *a = (RNetIceAgent *)user_ptr;
    (void)agent;
    if (a->force_relay && !ice_candidate_is_relay(sdp))
        return; /* Drop host/srflx so ICE must use TURN */
    ice_mutex_lock(&a->mutex);
    queue_cand(a, sdp);
    ice_mutex_unlock(&a->mutex);
}

static void on_gathering_done(juice_agent_t *agent, void *user_ptr)
{
    RNetIceAgent *a = (RNetIceAgent *)user_ptr;
    (void)agent;
    ice_mutex_lock(&a->mutex);
    a->gathering_done_posted = 1;
    ice_mutex_unlock(&a->mutex);
}

static void on_recv(juice_agent_t *agent, const char *data, size_t size, void *user_ptr)
{
    RNetIceAgent *a = (RNetIceAgent *)user_ptr;
    (void)agent;
    ice_mutex_lock(&a->mutex);
    queue_recv(a, data, size);
    ice_mutex_unlock(&a->mutex);
}

static void emit_signal(RNetIceAgent *a, RNetSignalType type, const char *text, rnet_u8 flag)
{
    RNetSignal msg;
    if (a->emit == NULL)
    {
        return;
    }
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    msg.flag = flag;
    if (text != NULL)
    {
        snprintf(msg.text, sizeof(msg.text), "%s", text);
    }
    a->emit(&msg, a->user);
}

RNetIceAgent *rnet_ice_agent_create(const RNetIceConfig *cfg, RNetIceSignalEmitFn emit, void *user)
{
    RNetIceAgent *a;
    juice_config_t jcfg;
    if (cfg == NULL)
    {
        return NULL;
    }
    a = (RNetIceAgent *)calloc(1, sizeof(*a));
    if (a == NULL)
    {
        return NULL;
    }
    ice_mutex_init(&a->mutex);
    a->emit = emit;
    a->user = user;
    a->state = RNET_ICE_STATE_IDLE;

    memset(&jcfg, 0, sizeof(jcfg));
    if (cfg->stun_host != NULL && cfg->stun_host[0] != '\0')
    {
        snprintf(a->stun_host, sizeof(a->stun_host), "%s", cfg->stun_host);
        jcfg.stun_server_host = a->stun_host;
        jcfg.stun_server_port = cfg->stun_port ? cfg->stun_port : 3478;
    }
    if (cfg->turn_host != NULL && cfg->turn_host[0] != '\0' && cfg->turn_user != NULL && cfg->turn_pass != NULL)
    {
        memset(&a->turn, 0, sizeof(a->turn));
        snprintf(a->turn_host, sizeof(a->turn_host), "%s", cfg->turn_host);
        snprintf(a->turn_user, sizeof(a->turn_user), "%s", cfg->turn_user);
        snprintf(a->turn_pass, sizeof(a->turn_pass), "%s", cfg->turn_pass);
        a->turn.host = a->turn_host;
        a->turn.port = cfg->turn_port ? cfg->turn_port : 3478;
        a->turn.username = a->turn_user;
        a->turn.password = a->turn_pass;
        jcfg.turn_servers = &a->turn;
        jcfg.turn_servers_count = 1;
        a->turn_count = 1;
    }
    if (cfg->bind_address != NULL && cfg->bind_address[0] != '\0')
    {
        snprintf(a->bind_address, sizeof(a->bind_address), "%s", cfg->bind_address);
        jcfg.bind_address = a->bind_address;
    }
    if (cfg->bind_port != 0)
    {
        jcfg.local_port_range_begin = cfg->bind_port;
        jcfg.local_port_range_end = cfg->bind_port;
    }
    jcfg.cb_state_changed = on_state_changed;
    jcfg.cb_candidate = on_candidate;
    jcfg.cb_gathering_done = on_gathering_done;
    jcfg.cb_recv = on_recv;
    jcfg.user_ptr = a;

    a->agent = juice_create(&jcfg);
    if (a->agent == NULL)
    {
        ice_mutex_destroy(&a->mutex);
        free(a);
        return NULL;
    }
    a->force_relay = cfg->force_relay ? 1 : 0;
#if defined(RNET_ICE_FORCE_TURN)
    a->force_relay = 1;
#endif
    if (a->force_relay)
    {
        if (a->turn_count == 0)
        {
            fprintf(stderr,
                    "rnet_ice: force_relay set but no TURN server in "
                    "RNetIceConfig — agent created without relay\n");
        }
        else
        {
            fprintf(stderr,
                    "rnet_ice: force_relay — only typ relay candidates will be "
                    "used\n");
        }
    }
    /* libjuice has no public set_ice_controlling; role follows offer/answer:
     * controlling gathers immediately; controlled waits for remote SDP. */
    a->controlling = cfg->controlling ? 1 : 0;
    return a;
}

void rnet_ice_agent_destroy(RNetIceAgent *agent)
{
    if (agent == NULL)
    {
        return;
    }
    if (agent->agent != NULL)
    {
        juice_destroy(agent->agent);
        agent->agent = NULL;
    }
    ice_mutex_destroy(&agent->mutex);
    free(agent);
}

static int ice_gather_now(RNetIceAgent *agent)
{
    char local_sdp[JUICE_MAX_SDP_STRING_LEN];
    if (agent->gather_started)
    {
        return 0;
    }
    if (juice_gather_candidates(agent->agent) < 0)
    {
        return -1;
    }
    agent->gather_started = 1;
    agent->gather_pending = 0;
    if (juice_get_local_description(agent->agent, local_sdp, sizeof(local_sdp)) == 0)
    {
        emit_signal(agent, RNET_SIGNAL_LOCAL_SDP, local_sdp, 0);
    }
    return 0;
}

int rnet_ice_agent_start_gathering(RNetIceAgent *agent)
{
    if ((agent == NULL) || (agent->agent == NULL))
    {
        return -1;
    }
    if (!agent->controlling && !agent->remote_desc_set)
    {
        /* Answerer: gather after remote offer arrives (see push_signal). */
        agent->gather_pending = 1;
        return 0;
    }
    return ice_gather_now(agent);
}

void rnet_ice_agent_poll(RNetIceAgent *agent)
{
    RNetIceCandSlot drain[RNET_ICE_CAND_QUEUE];
    unsigned count = 0;
    unsigned i;
    int gathering_done = 0;

    if (agent == NULL)
    {
        return;
    }

    ice_mutex_lock(&agent->mutex);
    while (agent->cand_count > 0 && count < RNET_ICE_CAND_QUEUE)
    {
        drain[count++] = agent->cand_q[agent->cand_head];
        agent->cand_head = (agent->cand_head + 1U) % RNET_ICE_CAND_QUEUE;
        agent->cand_count--;
    }
    if (agent->gathering_done_posted)
    {
        gathering_done = 1;
        agent->gathering_done_posted = 0;
    }
    ice_mutex_unlock(&agent->mutex);

    for (i = 0; i < count; ++i)
    {
        emit_signal(agent, RNET_SIGNAL_LOCAL_CANDIDATE, drain[i].sdp, 0);
    }
    if (gathering_done)
    {
        emit_signal(agent, RNET_SIGNAL_GATHERING_DONE, "", 0);
    }
}

void rnet_ice_agent_push_signal(RNetIceAgent *agent, const RNetSignal *msg)
{
    if ((agent == NULL) || (agent->agent == NULL) || (msg == NULL))
    {
        return;
    }
    switch (msg->type)
    {
    case RNET_SIGNAL_REMOTE_SDP:
        if (msg->text[0] != '\0')
        {
            if (juice_set_remote_description(agent->agent, msg->text) == 0)
            {
                agent->remote_desc_set = 1;
                if (agent->gather_pending || !agent->controlling)
                {
                    (void)ice_gather_now(agent);
                }
            }
        }
        break;
    case RNET_SIGNAL_REMOTE_CANDIDATE:
        if (msg->text[0] != '\0')
        {
            if (agent->force_relay && !ice_candidate_is_relay(msg->text))
                break;
            (void)juice_add_remote_candidate(agent->agent, msg->text);
        }
        break;
    case RNET_SIGNAL_GATHERING_DONE:
        (void)juice_set_remote_gathering_done(agent->agent);
        break;
    case RNET_SIGNAL_SET_CONTROLLING:
        /* Advisory for gather order; libjuice resolves role via SDP/conflicts. */
        agent->controlling = msg->flag ? 1 : 0;
        if (agent->controlling && agent->gather_pending)
        {
            (void)ice_gather_now(agent);
        }
        break;
    default:
        break;
    }
}

RNetIceState rnet_ice_agent_state(const RNetIceAgent *agent)
{
    if (agent == NULL)
    {
        return RNET_ICE_STATE_IDLE;
    }
    return agent->state;
}

int rnet_ice_agent_send(RNetIceAgent *agent, const rnet_u8 *buf, size_t len)
{
    int rc;
    if ((agent == NULL) || (agent->agent == NULL) || (buf == NULL) || (len == 0))
    {
        return -1;
    }
    rc = juice_send(agent->agent, (const char *)buf, len);
    if (rc < 0)
    {
        return -1;
    }
    return (int)len;
}

int rnet_ice_agent_recv(RNetIceAgent *agent, rnet_u8 *buf, size_t cap, size_t *out_len)
{
    if ((agent == NULL) || (buf == NULL) || (cap == 0) || (out_len == NULL))
    {
        return -1;
    }
    *out_len = 0;
    ice_mutex_lock(&agent->mutex);
    if (agent->recv_count == 0)
    {
        ice_mutex_unlock(&agent->mutex);
        return -1; /* empty */
    }
    {
        RNetIceRecvSlot *slot = &agent->recv_q[agent->recv_head];
        size_t n = slot->len;
        if (n > cap)
        {
            n = cap;
        }
        memcpy(buf, slot->data, n);
        *out_len = n;
        agent->recv_head = (agent->recv_head + 1U) % RNET_ICE_RECV_QUEUE;
        agent->recv_count--;
    }
    ice_mutex_unlock(&agent->mutex);
    return 0;
}

#endif /* RNET_ENABLE_ICE */
