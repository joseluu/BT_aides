/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Host ASHA (Audio Streaming for Hearing Aids) sur ESP32-S3 : capture audio
 * PCM1808 -> G.722 -> deux aides auditives (une par oreille), en stereo.
 * Base d'origine : exemple ESP-IDF ble_l2cap_coc/coc_blecent, tres remanie.
 * Protocole : docs/protocole_asha_reference.md ; historique des essais :
 * docs/plan_implementation.md.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
/* BLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "services/gap/ble_svc_gap.h"
#include "coc_blecent.h"
#include "audio_capture.h"
#include "driver/temperature_sensor.h"

static const char *tag = "NimBLE_BLE_CENT_L2CAP_COC";
static int blecent_gap_event(struct ble_gap_event *event, void *arg);

void ble_store_config_init(void);

#if MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM) < 2
#error "Deux canaux L2CAP CoC sont necessaires (un par aide) : CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM >= 2"
#endif

/* Par canal : une trame audio retenue par NimBLE (ESTALLED), une en
 * construction, un SDU de reception, plus une marge pour la reouverture. */
#define COC_BUF_COUNT          (4 * MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM))
#define MTU                    512

/* 1 : voie gauche du PCM1808 -> aide gauche, voie droite -> aide droite.
 * 0 : melange (G+D)/2 envoye aux deux aides. Avec une seule aide connectee,
 * c'est toujours le melange. */
#ifndef ASHA_STEREO
#define ASHA_STEREO 1
#endif

/* ==================== Protocole ASHA ====================
 * UUIDs et sequence issus de docs/protocole_asha_reference.md (mesures sur
 * les aides Signia via nRF Connect + sniff HCI Android). Octets des UUID 128
 * bits donnes dans l'ordre attendu par BLE_UUID128_INIT (inverse du texte). */

static const ble_uuid16_t asha_svc_uuid = BLE_UUID16_INIT(0xFDF0);

/* ReadOnlyProperties: 6333651e-c481-4a3e-9169-7c902aad37bb */
static const ble_uuid128_t asha_readonly_props_uuid = BLE_UUID128_INIT(
    0xbb, 0x37, 0xad, 0x2a, 0x90, 0x7c, 0x69, 0x91,
    0x3e, 0x4a, 0x81, 0xc4, 0x1e, 0x65, 0x33, 0x63);
/* AudioControlPoint: f0d4de7e-4a88-476c-9d9f-1937b0996cc0 */
static const ble_uuid128_t asha_audio_control_point_uuid = BLE_UUID128_INIT(
    0xc0, 0x6c, 0x99, 0xb0, 0x37, 0x19, 0x9f, 0x9d,
    0x6c, 0x47, 0x88, 0x4a, 0x7e, 0xde, 0xd4, 0xf0);
/* AudioStatusPoint: 38663f1a-e711-4cac-b641-326b56404837 */
static const ble_uuid128_t asha_audio_status_point_uuid = BLE_UUID128_INIT(
    0x37, 0x48, 0x40, 0x56, 0x6b, 0x32, 0x41, 0xb6,
    0xac, 0x4c, 0x11, 0xe7, 0x1a, 0x3f, 0x66, 0x38);
/* Volume: 00e4ca9e-ab14-41e4-8823-f9e70c7e91df */
static const ble_uuid128_t asha_volume_uuid = BLE_UUID128_INIT(
    0xdf, 0x91, 0x7e, 0x0c, 0xe7, 0xf9, 0x23, 0x88,
    0xe4, 0x41, 0x14, 0xab, 0x9e, 0xca, 0xe4, 0x00);
/* LE_PSM_OUT: 2d410339-82b6-42aa-b34e-e2e01df8cc1a */
static const ble_uuid128_t asha_le_psm_out_uuid = BLE_UUID128_INIT(
    0x1a, 0xcc, 0xf8, 0x1d, 0xe0, 0xe2, 0x4e, 0xb3,
    0xaa, 0x42, 0xb6, 0x82, 0x39, 0x03, 0x41, 0x2d);
static const ble_uuid16_t asha_cccd_uuid = BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);

/* Trame ASHA applicative = {seq (1 octet), 160 octets G.722} = 161 octets,
 * SANS prefixe : les deux octets "a1 00" vus dans le sniff HCI sont le champ
 * de longueur SDU du L2CAP CoC, ajoute par NimBLE dans ble_l2cap_send(). Les
 * construire a la main decalait la trame de 2 octets et l'aide coupait le
 * lien (bug trouve le 2026-09-13 via asha_pipewire_sink, AudioPacket.hh). */
#define ASHA_FRAME_PAYLOAD_LEN    160
#define ASHA_FRAME_LEN            (1 + ASHA_FRAME_PAYLOAD_LEN) /* 161 : seq + payload */
_Static_assert(ASHA_FRAME_PAYLOAD_LEN == AUDIO_G722_FRAME_BYTES, "trame ASHA = trame G.722 de 20 ms");

/* AudioControlPoint : opcodes (asha_pipewire_sink, Side.cxx). */
#define ASHA_OP_START             0x01  /* codec, audiotype, volume, otherstate */
#define ASHA_OP_STOP              0x02
#define ASHA_OP_STATUS            0x03  /* suivi d'un code de mise a jour */
#define ASHA_UPD_OTHER_DISCONNECTED 0x00
#define ASHA_UPD_OTHER_CONNECTED    0x01
/* ASHA_UPD_PARAMETERS_UPDATED (0x02) volontairement absent : `03 02 10`
 * envoye avant l'ouverture du CoC faisait echouer celle-ci (LE COC error 6). */

/* 0 dB (volume max, plage ASHA 0 a -128) ; -40 dB (0xd8) dans le sniff Android. */
static const uint8_t g_asha_volume_db = 0x00;

/* Le compteur de sequence redemarre a 3 a chaque START (sniff HCI Android). */
#define ASHA_SEQ_FIRST            3

/* Paire ciblee, choisie a la compilation : 1 = paire principale (HiSyncId
 * 30454345), 2 = paire secondaire (HiSyncId complet 9502464234444331, gauche
 * capability 0x02, droite 0x03 ; validee en direct le 2026-10-09). Les adresses
 * de ces aides sont des adresses privees qui tournent en 1-2 min : seul le
 * HiSyncId identifie la paire. */
#ifndef ASHA_TARGET_PAIR
#define ASHA_TARGET_PAIR 1
#endif
#if ASHA_TARGET_PAIR == 2
static const uint8_t g_asha_expected_truncated_hisyncid[4] = {0x34, 0x44, 0x43, 0x31};
#else
static const uint8_t g_asha_expected_truncated_hisyncid[4] = {0x30, 0x45, 0x43, 0x45};
#endif

/* ==================== Contexte par aide ==================== */

#define ASHA_EAR_UNKNOWN  -1
#define ASHA_EAR_LEFT      0
#define ASHA_EAR_RIGHT     1
#define ASHA_SLOT_COUNT    2

enum asha_state {
    ASHA_STATE_FREE,               /* emplacement libre */
    ASHA_STATE_SETUP,              /* connecte : bonding, decouverte, ouverture CoC */
    ASHA_STATE_READY,              /* CoC ouvert, notifications actives, pas de flux */
    ASHA_STATE_AWAITING_START_ACK,
    ASHA_STATE_STREAMING,
    ASHA_STATE_AWAITING_STOP_ACK,
    ASHA_STATE_REOPENING,          /* canal CoC bloque, ferme puis rouvert */
};

struct asha_handles {
    uint16_t read_only_properties;
    uint16_t audio_control_point;
    uint16_t audio_status_point;
    uint16_t audio_status_point_cccd;
    uint16_t volume;
    uint16_t le_psm_out;
};

struct asha_side {
    enum asha_state state;
    uint16_t conn_handle;
    int ear;                       /* ASHA_EAR_* (annonce, puis ReadOnlyProperties) */
    struct asha_handles h;
    uint16_t psm;
    struct ble_l2cap_chan *chan;
    SemaphoreHandle_t tx_unstalled; /* donne par COC_TX_UNSTALLED / COC_DISCONNECTED */
    bool stalled;
    int consecutive_failures;
    unsigned stall_count;
    bool reopen_pending;           /* reouverture CoC programmee (g_reopen_co) */
    bool in_group;                 /* deja annoncee prete au groupe (reste vrai
                                    * pendant une reouverture du CoC) */
};

static struct asha_side g_sides[ASHA_SLOT_COUNT];
/* Reouverture differee du canal CoC, une par emplacement (hors de asha_side :
 * side_release l'efface avec memset). */
static struct ble_npl_callout g_reopen_co[ASHA_SLOT_COUNT];

/* Protege g_sides, g_seq et les decisions d'orchestration, partages entre la
 * tache hote NimBLE (callbacks), la tache d'envoi audio et le minuteur de
 * redemarrage. Jamais tenu pendant une attente de credits. Recursif : les
 * callbacks NimBLE peuvent etre rappeles dans la tache qui tient deja le
 * verrou. Origine : coc_chan remis a NULL par COC_DISCONNECTED pendant un
 * ble_l2cap_send() -> LoadProhibited a l'adresse 0x38 (&chan->coc_tx),
 * observe a la perte de portee le 2026-10-09. */
static SemaphoreHandle_t g_lock;
#define LOCK()   xSemaphoreTakeRecursive(g_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(g_lock)

static uint8_t g_seq;
/* Une connexion GAP est en cours d'etablissement (scan arrete pendant ce temps). */
static bool g_connecting;
static int g_connecting_ear = ASHA_EAR_UNKNOWN;
/* Un redemarrage du groupe attend les ack de STOP (voir asha_group_update). */
static bool g_restart_pending;
static esp_timer_handle_t g_restart_timer;
/* Mode du scan en cours (lent des qu'une aide est en flux). */
static bool g_scan_slow;

static os_membuf_t sdu_coc_mem[OS_MEMPOOL_SIZE(COC_BUF_COUNT, MTU * 2)];
static struct os_mempool sdu_coc_mbuf_mempool;
static struct os_mbuf_pool sdu_os_mbuf_pool;
static int blecent_l2cap_coc_event_cb(struct ble_l2cap_event *event, void *arg);
static void blecent_scan(void);
static void asha_group_update(void);

static const char *
ear_name(const struct asha_side *s)
{
    return s->ear == ASHA_EAR_LEFT ? "G" : (s->ear == ASHA_EAR_RIGHT ? "D" : "?");
}

static struct asha_side *
side_by_conn(uint16_t conn_handle)
{
    for (int i = 0; i < ASHA_SLOT_COUNT; i++) {
        if (g_sides[i].state != ASHA_STATE_FREE && g_sides[i].conn_handle == conn_handle) {
            return &g_sides[i];
        }
    }
    return NULL;
}

static struct asha_side *
side_free_slot(void)
{
    for (int i = 0; i < ASHA_SLOT_COUNT; i++) {
        if (g_sides[i].state == ASHA_STATE_FREE) {
            return &g_sides[i];
        }
    }
    return NULL;
}

/* Echec pendant la configuration d'une aide : on coupe la connexion GAP, et
 * BLE_GAP_EVENT_DISCONNECT libere l'emplacement (sinon il resterait en SETUP
 * et bloquerait le scan pour l'autre cote). */
/* Diagnostic au sniffer nRF (--irk-key / --sc-ltk-key) : affiche l'IRK et la
 * LTK de l'aide apres le chiffrement. Les bonds ne sont qu'en RAM
 * (NVS_PERSIST desactive) : la LTK reste valable jusqu'au prochain reset de
 * l'ESP32, reconnexions comprises. A ne jamais activer hors diagnostic. */
#ifndef ASHA_LOG_BOND_KEYS
#define ASHA_LOG_BOND_KEYS 0
#endif

#if ASHA_LOG_BOND_KEYS
static void
log_key_be(const char *name, const char *ear, const uint8_t *k)
{
    char hex[2 * 16 + 1];
    /* NimBLE stocke les cles en little-endian ; nrfutil les attend en
     * big-endian, prefixees par 0x. */
    for (int i = 0; i < 16; i++) {
        sprintf(&hex[2 * i], "%02x", k[15 - i]);
    }
    MODLOG_DFLT(WARN, "ASHA[%s]: CLE %s 0x%s\n", ear, name, hex);
}

static void
log_bond_keys(const struct asha_side *s)
{
    struct ble_gap_conn_desc desc;
    struct ble_store_key_sec key = {0};
    struct ble_store_value_sec val;

    if (ble_gap_conn_find(s->conn_handle, &desc) != 0) {
        return;
    }
    key.peer_addr = desc.peer_id_addr;
    if (ble_store_read_peer_sec(&key, &val) != 0) {
        MODLOG_DFLT(WARN, "ASHA[%s]: cles du bond introuvables\n", ear_name(s));
        return;
    }
    MODLOG_DFLT(WARN, "ASHA[%s]: identite %02x:%02x:%02x:%02x:%02x:%02x (type %d), sc=%d\n",
                ear_name(s), val.peer_addr.val[5], val.peer_addr.val[4], val.peer_addr.val[3],
                val.peer_addr.val[2], val.peer_addr.val[1], val.peer_addr.val[0],
                val.peer_addr.type, val.sc);
    if (val.irk_present) {
        log_key_be("IRK", ear_name(s), val.irk);
    }
    if (val.ltk_present) {
        log_key_be("LTK", ear_name(s), val.ltk);
    }
}
#endif

static void
side_fail(struct asha_side *s, const char *why)
{
    MODLOG_DFLT(ERROR, "ASHA: %s -> deconnexion de cette aide\n", why);
    ble_gap_terminate(s->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

static bool
ear_taken(int ear)
{
    for (int i = 0; i < ASHA_SLOT_COUNT; i++) {
        if (g_sides[i].state != ASHA_STATE_FREE && g_sides[i].ear == ear) {
            return true;
        }
    }
    return false;
}

static int
count_in_state(enum asha_state st)
{
    int n = 0;
    for (int i = 0; i < ASHA_SLOT_COUNT; i++) {
        n += g_sides[i].state == st;
    }
    return n;
}

static void
side_release(struct asha_side *s)
{
    SemaphoreHandle_t sem = s->tx_unstalled;
    memset(s, 0, sizeof(*s));
    s->state = ASHA_STATE_FREE;
    s->ear = ASHA_EAR_UNKNOWN;
    s->tx_unstalled = sem;
    /* Debloque une tache d'envoi qui attendrait encore des credits. */
    xSemaphoreGive(sem);
}

/* ==================== Commandes AudioControlPoint ==================== */

static void
asha_send_status(struct asha_side *s, uint8_t update)
{
    uint8_t cmd[2] = {ASHA_OP_STATUS, update};
    int rc = ble_gattc_write_no_rsp_flat(s->conn_handle, s->h.audio_control_point, cmd, sizeof(cmd));
    MODLOG_DFLT(INFO, "ASHA[%s]: STATUS autre cote %s, rc=%d\n", ear_name(s),
                update == ASHA_UPD_OTHER_CONNECTED ? "connecte" : "deconnecte", rc);
}

static void
asha_send_stop(struct asha_side *s)
{
    uint8_t cmd = ASHA_OP_STOP;
    int rc = ble_gattc_write_no_rsp_flat(s->conn_handle, s->h.audio_control_point, &cmd, sizeof(cmd));
    MODLOG_DFLT(INFO, "ASHA[%s]: envoi STOP, rc=%d\n", ear_name(s), rc);
}

static int
asha_on_start_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    LOCK();
    struct asha_side *s = side_by_conn(conn_handle);
    if (s != NULL && error->status != 0) {
        MODLOG_DFLT(ERROR, "ASHA[%s]: envoi START echoue, status=%d\n", ear_name(s), error->status);
        s->state = ASHA_STATE_READY;
    } else if (s != NULL) {
        MODLOG_DFLT(INFO, "ASHA[%s]: START envoye, attente de l'ack\n", ear_name(s));
    }
    UNLOCK();
    return 0;
}

/* START sur une aide READY. Precede de `03 0x` (autre cote connecte ou non) :
 * l'ecriture `03 00` du sniff Android mono-aide est ce message STATUS. */
static void
asha_send_start(struct asha_side *s, bool other_connected)
{
    asha_send_status(s, other_connected ? ASHA_UPD_OTHER_CONNECTED : ASHA_UPD_OTHER_DISCONNECTED);

    uint8_t cmd[5] = {
        ASHA_OP_START,
        0x01,                   /* codec G.722 16 kHz */
        0x00,                   /* audio type */
        g_asha_volume_db,
        other_connected ? 0x01 : 0x00, /* otherstate : 0 en mono-aide (sniff Android) */
    };
    s->state = ASHA_STATE_AWAITING_START_ACK;
    int rc = ble_gattc_write_flat(s->conn_handle, s->h.audio_control_point, cmd, sizeof(cmd),
                                  asha_on_start_write, NULL);
    MODLOG_DFLT(INFO, "ASHA[%s]: START (otherstate=%d), rc=%d\n", ear_name(s), cmd[4], rc);
    if (rc != 0) {
        s->state = ASHA_STATE_READY;
    }
}

/* ==================== Orchestration du groupe ====================
 * Schema "tout redemarrer" d'asha_pipewire_sink (Device.cxx) : quand une aide
 * arrive ou part pendant le flux, on arrete toutes les aides en cours, puis on
 * redemarre toutes les aides pretes ensemble — encodeurs reinitialises,
 * sequence commune repartie a 3, otherstate selon le nombre d'aides. L'auteur
 * de ce projet note que garder une aide en flux pendant le changement marchait
 * mal. */

static void
asha_start_all_ready(void)
{
    int n = count_in_state(ASHA_STATE_READY);
    if (n == 0) {
        return;
    }
    bool stereo = ASHA_STEREO && n == ASHA_SLOT_COUNT;
    audio_capture_set_stereo(stereo);
    audio_capture_restart_stream();
    g_seq = ASHA_SEQ_FIRST;
    MODLOG_DFLT(INFO, "ASHA: demarrage du groupe, %d aide(s), %s\n", n, stereo ? "stereo" : "mono");
    for (int i = 0; i < ASHA_SLOT_COUNT; i++) {
        if (g_sides[i].state == ASHA_STATE_READY) {
            asha_send_start(&g_sides[i], n > 1);
        }
    }
}

/* Appele a chaque changement d'etat (aide prete, ack STOP, aide perdue). */
static void
asha_group_update(void)
{
    if (g_restart_pending) {
        if (count_in_state(ASHA_STATE_AWAITING_STOP_ACK) == 0) {
            g_restart_pending = false;
            esp_timer_stop(g_restart_timer);
            asha_start_all_ready();
        }
        return;
    }
    if (count_in_state(ASHA_STATE_READY) == 0) {
        return;
    }
    int running = count_in_state(ASHA_STATE_STREAMING) + count_in_state(ASHA_STATE_AWAITING_START_ACK);
    if (running == 0) {
        asha_start_all_ready();
        return;
    }
    /* Une aide prete rejoint un flux en cours : STOP a celles qui tournent,
     * redemarrage commun a la reception des ack (ou du minuteur). */
    MODLOG_DFLT(INFO, "ASHA: nouvelle aide prete pendant le flux, redemarrage du groupe\n");
    g_restart_pending = true;
    for (int i = 0; i < ASHA_SLOT_COUNT; i++) {
        struct asha_side *s = &g_sides[i];
        if (s->state == ASHA_STATE_STREAMING || s->state == ASHA_STATE_AWAITING_START_ACK) {
            s->state = ASHA_STATE_AWAITING_STOP_ACK;
            asha_send_stop(s);
        }
    }
    esp_timer_start_once(g_restart_timer, 1000 * 1000);
}

/* Filet de securite : une aide qui n'acquitte pas son STOP en 1 s ne doit pas
 * bloquer le redemarrage du groupe. */
static void
asha_restart_timeout(void *arg)
{
    LOCK();
    if (g_restart_pending) {
        for (int i = 0; i < ASHA_SLOT_COUNT; i++) {
            if (g_sides[i].state == ASHA_STATE_AWAITING_STOP_ACK) {
                MODLOG_DFLT(ERROR, "ASHA[%s]: pas d'ack de STOP en 1 s, consideree arretee\n",
                            ear_name(&g_sides[i]));
                g_sides[i].state = ASHA_STATE_READY;
            }
        }
        asha_group_update();
    }
    UNLOCK();
}

/* Une aide vient de disparaitre (GAP) : prevenir l'autre et la redemarrer
 * seule (otherstate=0, flux mono). */
static void
asha_on_side_lost(void)
{
    for (int i = 0; i < ASHA_SLOT_COUNT; i++) {
        struct asha_side *s = &g_sides[i];
        if (s->state == ASHA_STATE_STREAMING || s->state == ASHA_STATE_AWAITING_START_ACK) {
            asha_send_status(s, ASHA_UPD_OTHER_DISCONNECTED);
            s->state = ASHA_STATE_AWAITING_STOP_ACK;
            asha_send_stop(s);
            g_restart_pending = true;
        } else if (s->state == ASHA_STATE_READY || s->state == ASHA_STATE_SETUP) {
            asha_send_status(s, ASHA_UPD_OTHER_DISCONNECTED);
        }
    }
    if (g_restart_pending) {
        esp_timer_start_once(g_restart_timer, 1000 * 1000);
    }
}

/* Relance le scan si un emplacement est libre et qu'aucune connexion n'est en
 * cours d'etablissement ou de configuration (l'aide n'accepte l'ouverture du
 * CoC que peu de temps apres la connexion : ne pas lui voler de temps radio). */
static void
asha_maybe_scan(void)
{
    if (g_connecting || side_free_slot() == NULL || count_in_state(ASHA_STATE_SETUP) > 0) {
        return;
    }
    if (ble_gap_disc_active()) {
        bool want_slow = count_in_state(ASHA_STATE_STREAMING) > 0;
        if (want_slow == g_scan_slow) {
            return;
        }
        /* Une aide vient d'entrer en flux : repasser en scan lent. */
        ble_gap_disc_cancel();
    }
    blecent_scan();
}

/* ==================== Tache d'envoi audio ====================
 * Permanente (creee au demarrage). A chaque trame de 20 ms (cadence par le
 * quartz audio via la file de audio_capture), envoie le meme numero de
 * sequence a chaque aide en flux : trame gauche a l'aide gauche, droite a la
 * droite. Une aide a court de credits bloque la paire (attente de son
 * TX_UNSTALLED) : la file deborde alors cote capture, qui saute le PCM des
 * deux voies AVANT encodage — jamais de trame encodee jetee pour une seule
 * oreille (G.722 est differentiel). */

#define ASHA_MAX_CONSECUTIVE_FRAME_FAILURES 30

/* Canal CoC mort (credits jamais rendus, observe en test reel : ne recupere
 * jamais seul). On referme et rouvre UNIQUEMENT le canal CoC, sans toucher a
 * la connexion GAP : apres une deconnexion GAP l'aide ne re-annonce plus avant
 * un off/on manuel (doc AOSP : le central ne doit pas se deconnecter). */
static void
asha_side_reopen(struct asha_side *s, const char *why)
{
    MODLOG_DFLT(ERROR, "ASHA[%s]: %s -> fermeture/reouverture du canal CoC"
                " (connexion GAP conservee)\n", ear_name(s), why);
    s->state = ASHA_STATE_REOPENING;
    asha_send_stop(s);
    if (s->chan != NULL) {
        ble_l2cap_disconnect(s->chan);
    }
}

/* Envoie sdu sur le canal de s s'il existe encore ; BLE_HS_ENOTCONN sinon
 * (sdu non consomme, a liberer par l'appelant). */
static int
asha_send_locked(struct asha_side *s, struct os_mbuf *sdu)
{
    int rc;
    LOCK();
    rc = (s->state == ASHA_STATE_STREAMING && s->chan != NULL) ? ble_l2cap_send(s->chan, sdu)
                                                               : BLE_HS_ENOTCONN;
    UNLOCK();
    return rc;
}

static void
asha_send_frame(struct asha_side *s, uint8_t seq, const uint8_t *payload)
{
    uint8_t frame[ASHA_FRAME_LEN];
    struct os_mbuf *sdu;
    int rc;

    if (s->stalled) {
        if (xSemaphoreTake(s->tx_unstalled, pdMS_TO_TICKS(1000)) != pdTRUE) {
            LOCK();
            if (s->state == ASHA_STATE_STREAMING) {
                asha_side_reopen(s, "credits CoC non rendus depuis 1 s");
            }
            UNLOCK();
            s->stalled = false;
            return;
        }
        s->stalled = false;
        if (++s->stall_count % 50 == 1) {
            MODLOG_DFLT(INFO, "ASHA[%s]: attente de credits CoC (%u fois)\n", ear_name(s), s->stall_count);
        }
    }

    frame[0] = seq;
    memcpy(&frame[1], payload, ASHA_FRAME_PAYLOAD_LEN);
    do {
        sdu = os_mbuf_get_pkthdr(&sdu_os_mbuf_pool, 0);
        if (sdu == NULL) {
            vTaskDelay(2 / portTICK_PERIOD_MS);
        }
    } while (sdu == NULL);
    os_mbuf_append(sdu, frame, sizeof(frame));

    /* ESTALLED = trame ACCEPTEE : NimBLE la garde et l'enverra quand l'aide
     * rendra des credits ; ne ni la renvoyer ni la liberer, attendre
     * TX_UNSTALLED avant la suivante. EBUSY = refusee (une autre en cours) :
     * on retente brievement puis on la libere. */
    rc = asha_send_locked(s, sdu);
    for (int retry = 0; rc == BLE_HS_EBUSY && retry < 20; retry++) {
        vTaskDelay(5 / portTICK_PERIOD_MS);
        rc = asha_send_locked(s, sdu);
    }
    if (rc == BLE_HS_ESTALLED) {
        s->stalled = true;
        s->consecutive_failures = 0;
    } else if (rc == 0) {
        s->consecutive_failures = 0;
    } else {
        os_mbuf_free_chain(sdu);
        if (rc == BLE_HS_ENOTCONN) {
            return;          /* aide sortie du flux entre-temps */
        }
        s->consecutive_failures++;
        MODLOG_DFLT(ERROR, "ASHA[%s]: envoi trame echoue, rc=%d (%d echecs consecutifs)\n",
                    ear_name(s), rc, s->consecutive_failures);
        if (s->consecutive_failures >= ASHA_MAX_CONSECUTIVE_FRAME_FAILURES) {
            s->consecutive_failures = 0;
            LOCK();
            if (s->state == ASHA_STATE_STREAMING) {
                asha_side_reopen(s, "canal CoC bloque");
            }
            UNLOCK();
        }
    }
}

static void
asha_audio_stream_task(void *arg)
{
    struct audio_frame_pair pair;

    while (1) {
        if (!audio_capture_get_frame_pair(&pair, pdMS_TO_TICKS(100))) {
            continue;
        }

        struct asha_side *targets[ASHA_SLOT_COUNT];
        int n = 0;
        uint8_t seq;
        LOCK();
        for (int i = 0; i < ASHA_SLOT_COUNT; i++) {
            if (g_sides[i].state == ASHA_STATE_STREAMING) {
                targets[n++] = &g_sides[i];
            }
        }
        seq = g_seq;
        if (n > 0) {
            g_seq++;
        }
        UNLOCK();

        for (int k = 0; k < n; k++) {
            struct asha_side *s = targets[k];
            asha_send_frame(s, seq, s->ear == ASHA_EAR_RIGHT ? pair.right : pair.left);
        }
    }
}

/* ==================== Sequence de connexion d'une aide ==================== */

static int
asha_on_cccd_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg)
{
    LOCK();
    struct asha_side *s = side_by_conn(conn_handle);
    if (s == NULL) {
        UNLOCK();
        return 0;
    }
    if (error->status != 0) {
        MODLOG_DFLT(ERROR, "ASHA[%s]: activation notifications AudioStatusPoint echouee, status=%d\n",
                    ear_name(s), error->status);
        side_fail(s, "CCCD AudioStatusPoint");
        UNLOCK();
        return 0;
    }

    /* Volume est "Write No Response" uniquement (sniff Android) : un write
     * classique est rejete (status=508). */
    int rc = ble_gattc_write_no_rsp_flat(conn_handle, s->h.volume,
                                         &g_asha_volume_db, sizeof(g_asha_volume_db));
    MODLOG_DFLT(INFO, "ASHA[%s]: notifications actives, Volume rc=%d, aide prete\n", ear_name(s), rc);
    s->state = ASHA_STATE_READY;
    s->in_group = true;
    asha_group_update();
    asha_maybe_scan();
    UNLOCK();
    return 0;
}

static int
asha_on_le_psm_out_read(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    uint8_t buf[2];
    struct os_mbuf *sdu_rx;

    LOCK();
    struct asha_side *s = side_by_conn(conn_handle);
    if (s == NULL) {
        UNLOCK();
        return 0;
    }
    if (error->status != 0 || OS_MBUF_PKTLEN(attr->om) < 2) {
        MODLOG_DFLT(ERROR, "ASHA[%s]: lecture LE_PSM_OUT echouee, status=%d\n", ear_name(s), error->status);
        side_fail(s, "LE_PSM_OUT");
        UNLOCK();
        return 0;
    }
    ble_hs_mbuf_to_flat(attr->om, buf, sizeof(buf), NULL);
    s->psm = buf[0] | (buf[1] << 8);

    /* Ordre du sniff Android : le canal CoC s'ouvre AVANT CCCD/Volume/START
     * (l'ouvrir apres START donnait LE COC error 6). La suite reprend sur
     * BLE_L2CAP_EVENT_COC_CONNECTED. */
    MODLOG_DFLT(INFO, "ASHA[%s]: PSM 0x%04x, ouverture du canal L2CAP CoC...\n", ear_name(s), s->psm);
    sdu_rx = os_mbuf_get_pkthdr(&sdu_os_mbuf_pool, 0);
    int rc = sdu_rx == NULL ? BLE_HS_ENOMEM
                            : ble_l2cap_connect(conn_handle, s->psm, MTU, sdu_rx,
                                                blecent_l2cap_coc_event_cb, s);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "ASHA[%s]: ouverture du canal CoC, rc=%d\n", ear_name(s), rc);
        if (sdu_rx != NULL) {
            os_mbuf_free_chain(sdu_rx);
        }
        side_fail(s, "ouverture du canal CoC");
    }
    UNLOCK();
    return 0;
}

static int
asha_on_readonly_props_read(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    uint8_t buf[17];
    int len;

    LOCK();
    struct asha_side *s = side_by_conn(conn_handle);
    if (s == NULL) {
        UNLOCK();
        return 0;
    }
    if (error->status != 0) {
        MODLOG_DFLT(ERROR, "ASHA[%s]: lecture ReadOnlyProperties echouee, status=%d\n",
                    ear_name(s), error->status);
        side_fail(s, "ReadOnlyProperties");
        UNLOCK();
        return 0;
    }

    len = OS_MBUF_PKTLEN(attr->om);
    if (len > (int)sizeof(buf)) {
        len = sizeof(buf);
    }
    ble_hs_mbuf_to_flat(attr->om, buf, len, NULL);

    if (len >= 17) {
        uint64_t hisyncid = 0;
        for (int i = 0; i < 8; i++) {
            hisyncid = (hisyncid << 8) | buf[2 + i];
        }
        uint16_t codecs = buf[15] | (buf[16] << 8);
        /* capability bit0 : 0 = gauche, 1 = droite (bit1 = binaural). */
        int ear = (buf[1] & 0x01) ? ASHA_EAR_RIGHT : ASHA_EAR_LEFT;
        if (s->ear != ASHA_EAR_UNKNOWN && s->ear != ear) {
            MODLOG_DFLT(ERROR, "ASHA: cote annonce et cote lu differents, cote lu retenu\n");
        }
        s->ear = ear;
        MODLOG_DFLT(INFO, "ASHA[%s] ReadOnlyProperties: version=%d capability=0x%02x "
                    "hisyncid=%016llx codecs=0x%04x\n",
                    ear_name(s), buf[0], buf[1], (unsigned long long)hisyncid, codecs);
        /* Connexion par liste blanche au demarrage : l'annonce n'a pas ete
         * filtree, on verifie ici la paire. buf[2..9] est le HiSyncId affiche
         * ci-dessus ; ses 4 derniers octets (buf[6..9]) sont ceux de l'annonce
         * (9502464234444331 <-> 34 44 43 31, verifie le 2026-10-09). */
        if (memcmp(&buf[6], g_asha_expected_truncated_hisyncid, 4) != 0) {
            side_fail(s, "aide d'une autre paire");
            UNLOCK();
            return 0;
        }
        /* Deux aides du meme cote : la seconde est inutile, on la libere. */
        for (int i = 0; i < ASHA_SLOT_COUNT; i++) {
            if (&g_sides[i] != s && g_sides[i].state != ASHA_STATE_FREE && g_sides[i].ear == ear) {
                side_fail(s, "cote deja occupe");
                UNLOCK();
                return 0;
            }
        }
    } else {
        MODLOG_DFLT(ERROR, "ASHA: ReadOnlyProperties trop courte (%d octets)\n", len);
    }

    ble_gattc_read(conn_handle, s->h.le_psm_out, asha_on_le_psm_out_read, NULL);
    UNLOCK();
    return 0;
}

/* Decouverte du service ASHA terminee : localise les characteristics par UUID
 * puis lit ReadOnlyProperties -> LE_PSM_OUT -> ouverture CoC. */
static void
blecent_l2cap_coc_on_disc_complete(const struct peer *peer, int status, void *arg)
{
    const struct peer_chr *chr;
    const struct peer_dsc *dsc;

    LOCK();
    struct asha_side *s = side_by_conn(peer->conn_handle);
    if (s == NULL) {
        UNLOCK();
        return;
    }
    MODLOG_DFLT(INFO, "ASHA[%s]: decouverte de services terminee, status=%d\n", ear_name(s), status);

#define FIND_CHR(field, uuid, label)                                              \
    chr = peer_chr_find_uuid(peer, &asha_svc_uuid.u, &(uuid).u);                  \
    if (chr == NULL) {                                                            \
        side_fail(s, "characteristic " label " introuvable");                     \
        UNLOCK();                                                                 \
        return;                                                                   \
    }                                                                             \
    s->h.field = chr->chr.val_handle;

    FIND_CHR(read_only_properties, asha_readonly_props_uuid, "ReadOnlyProperties");
    FIND_CHR(audio_control_point, asha_audio_control_point_uuid, "AudioControlPoint");
    FIND_CHR(audio_status_point, asha_audio_status_point_uuid, "AudioStatusPoint");
    FIND_CHR(volume, asha_volume_uuid, "Volume");
    FIND_CHR(le_psm_out, asha_le_psm_out_uuid, "LE_PSM_OUT");
#undef FIND_CHR

    dsc = peer_dsc_find_uuid(peer, &asha_svc_uuid.u, &asha_audio_status_point_uuid.u,
                             &asha_cccd_uuid.u);
    if (dsc == NULL) {
        side_fail(s, "descripteur CCCD d'AudioStatusPoint introuvable");
        UNLOCK();
        return;
    }
    s->h.audio_status_point_cccd = dsc->dsc.handle;

    ble_gattc_read(peer->conn_handle, s->h.read_only_properties, asha_on_readonly_props_read, NULL);
    UNLOCK();
}

static int
blecent_l2cap_coc_event_cb(struct ble_l2cap_event *event, void *arg)
{
    struct asha_side *s = arg;
    struct ble_l2cap_chan_info chan_info;

    switch (event->type) {
    case BLE_L2CAP_EVENT_COC_CONNECTED:
        LOCK();
        if (event->connect.status) {
            MODLOG_DFLT(ERROR, "ASHA[%s]: LE COC error: %d\n", ear_name(s), event->connect.status);
            if (s->state != ASHA_STATE_FREE) {
                side_fail(s, "ouverture du canal CoC");
            }
            UNLOCK();
            return 0;
        }
        if (ble_l2cap_get_chan_info(event->connect.chan, &chan_info) == 0) {
            MODLOG_DFLT(INFO, "ASHA[%s]: canal CoC ouvert, conn %d, peer_mps %d, peer_mtu %d\n",
                        ear_name(s), event->connect.conn_handle,
                        chan_info.peer_l2cap_mtu, chan_info.peer_coc_mtu);
        }
        s->chan = event->connect.chan;
        s->stalled = false;
        s->consecutive_failures = 0;
        xSemaphoreTake(s->tx_unstalled, 0);
        {
            uint16_t cccd_val = 1;
            ble_gattc_write_flat(s->conn_handle, s->h.audio_status_point_cccd,
                                 &cccd_val, sizeof(cccd_val), asha_on_cccd_write, NULL);
        }
        UNLOCK();
        return 0;

    case BLE_L2CAP_EVENT_COC_DISCONNECTED:
        LOCK();
        MODLOG_DFLT(INFO, "ASHA[%s]: canal CoC ferme\n", ear_name(s));
        s->chan = NULL;
        xSemaphoreGive(s->tx_unstalled);
        if (s->state != ASHA_STATE_FREE && s->state != ASHA_STATE_SETUP) {
            /* Fermeture demandee (canal bloque) ou par l'aide : on rouvre le
             * seul canal CoC, la connexion GAP reste. Jamais depuis ce
             * callback : NimBLE n'a pas encore libere l'ancien canal (pool
             * plein en stereo -> rc=6), et si la connexion GAP est en train
             * de tomber, BLE_GAP_EVENT_DISCONNECT n'est pas encore passe.
             * Reouvrir ici donnait des appels recursifs puis un LoadProhibited
             * dans ble_hs_conn_delete_chan (2026-10-11). Le callout s'execute
             * dans la tache hote apres la fin de ce traitement. */
            s->state = ASHA_STATE_SETUP;
            s->reopen_pending = true;
            ble_npl_callout_reset(&g_reopen_co[s - g_sides], 0);
        }
        UNLOCK();
        return 0;

    case BLE_L2CAP_EVENT_COC_TX_UNSTALLED:
        xSemaphoreGive(s->tx_unstalled);
        return 0;

    default:
        return 0;
    }
}

static void
asha_reopen_cb(struct ble_npl_event *ev)
{
    struct asha_side *s = ble_npl_event_get_arg(ev);

    LOCK();
    /* Emplacement libere (deconnexion GAP) ou reutilise entre-temps. */
    if (!s->reopen_pending || s->state != ASHA_STATE_SETUP || s->chan != NULL) {
        UNLOCK();
        return;
    }
    s->reopen_pending = false;
    struct os_mbuf *sdu_rx = os_mbuf_get_pkthdr(&sdu_os_mbuf_pool, 0);
    int rc = sdu_rx == NULL ? BLE_HS_ENOMEM
                            : ble_l2cap_connect(s->conn_handle, s->psm, MTU, sdu_rx,
                                                blecent_l2cap_coc_event_cb, s);
    MODLOG_DFLT(INFO, "ASHA[%s]: reouverture du canal CoC, rc=%d\n", ear_name(s), rc);
    if (rc != 0) {
        if (sdu_rx != NULL) {
            os_mbuf_free_chain(sdu_rx);
        }
        side_fail(s, "reouverture du canal CoC");
    }
    UNLOCK();
}

static void
blecent_l2cap_coc_mem_init(void)
{
    int rc;
    rc = os_mempool_init(&sdu_coc_mbuf_mempool, COC_BUF_COUNT, MTU, sdu_coc_mem,
                         "coc_sdu_pool");
    assert(rc == 0);
    rc = os_mbuf_pool_init(&sdu_os_mbuf_pool, &sdu_coc_mbuf_mempool, MTU,
                           COC_BUF_COUNT);
    assert(rc == 0);
}

/* ==================== Scan et connexion ==================== */

static void
blecent_scan(void)
{
    uint8_t own_addr_type;
    struct ble_gap_disc_params disc_params = {0};
    int rc;

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
        return;
    }

    /* Duplicates non filtres : le controleur pourrait sinon supprimer un
     * rapport qui porte les donnees ASHA. */
    disc_params.filter_duplicates = 0;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    /* Scan actif (le service data 0xFDF0 est dans l'ADV_IND sur les deux
     * paires, sniff 01_advertising_scan.pcap et 2026-10-09, mais l'actif
     * couvre aussi un SCAN_RSP). Une aide en flux : faible taux d'occupation
     * (30 ms toutes les 500 ms) pour ne pas affamer son lien, deja fragile a
     * quelques metres. */
    disc_params.passive = 0;
    if (count_in_state(ASHA_STATE_STREAMING) > 0) {
        disc_params.itvl = 800;   /* x 0,625 ms = 500 ms */
        disc_params.window = 48;  /* x 0,625 ms = 30 ms */
    } else {
        disc_params.itvl = 0;
        disc_params.window = 0;
    }

    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params,
                      blecent_gap_event, NULL);
    g_scan_slow = disc_params.itvl != 0;
    MODLOG_DFLT(INFO, "ASHA: scan %s lance, rc=%d\n", g_scan_slow ? "lent" : "continu", rc);
}

/* Renvoie le cote annonce (ASHA_EAR_*) si l'annonce vient d'une aide de la
 * paire ciblee dont le cote est encore libre, -2 sinon. */
static int
blecent_should_connect(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;

    if (disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
            disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND &&
            disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
        return -2;
    }
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return -2;
    }
    /* svc_data_uuid16 = [uuid_lo, uuid_hi, version, capability, hisyncid_tronque x4] */
    if (fields.svc_data_uuid16 == NULL || fields.svc_data_uuid16_len < 8 ||
            fields.svc_data_uuid16[0] != 0xF0 || fields.svc_data_uuid16[1] != 0xFD) {
        return -2;
    }
    if (memcmp(&fields.svc_data_uuid16[4], g_asha_expected_truncated_hisyncid, 4) != 0) {
        return -2;
    }
    int ear = (fields.svc_data_uuid16[3] & 0x01) ? ASHA_EAR_RIGHT : ASHA_EAR_LEFT;
    if (ear_taken(ear)) {
        return -2;
    }
    return ear;
}

static void
blecent_connect_if_interesting(const struct ble_gap_disc_desc *disc)
{
    uint8_t own_addr_type;
    int rc;

    if (g_connecting || side_free_slot() == NULL) {
        return;
    }
    int ear = blecent_should_connect(disc);
    if (ear == -2) {
        return;
    }
    MODLOG_DFLT(INFO, "ASHA: aide %s de la paire ciblee vue (rssi %d), connexion...\n",
                ear == ASHA_EAR_RIGHT ? "droite" : "gauche", disc->rssi);

    /* Le scan doit etre arrete avant d'initier une connexion. */
    rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        MODLOG_DFLT(DEBUG, "Failed to cancel scan; rc=%d\n", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
        return;
    }
    rc = ble_gap_connect(own_addr_type, &disc->addr, 30000, NULL, blecent_gap_event, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Error: Failed to connect to device; addr=%s; rc=%d\n",
                    addr_str(disc->addr.val), rc);
        asha_maybe_scan();
        return;
    }
    g_connecting = true;
    g_connecting_ear = ear;
}

static int
blecent_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    struct asha_side *s;
    int rc;

    LOCK();
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        {
            static uint32_t disc_count = 0;
            if (++disc_count % 1000 == 1) {
                MODLOG_DFLT(INFO, "Scan: %lu paquets recus (dernier addr=%s, "
                            "event_type=%d, rssi=%d)\n",
                            (unsigned long)disc_count, addr_str(event->disc.addr.val),
                            event->disc.event_type, event->disc.rssi);
            }
        }
        blecent_connect_if_interesting(&event->disc);
        break;

    case BLE_GAP_EVENT_CONNECT:
        g_connecting = false;
        if (event->connect.status != 0) {
            MODLOG_DFLT(ERROR, "Error: Connection failed; status=%d\n", event->connect.status);
            asha_maybe_scan();
            break;
        }
        s = side_free_slot();
        if (s == NULL) {
            MODLOG_DFLT(ERROR, "ASHA: connexion sans emplacement libre, deconnexion\n");
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        s->state = ASHA_STATE_SETUP;
        s->conn_handle = event->connect.conn_handle;
        s->ear = g_connecting_ear;
        g_connecting_ear = ASHA_EAR_UNKNOWN;
        MODLOG_DFLT(INFO, "ASHA[%s]: connexion etablie, conn %d\n", ear_name(s), s->conn_handle);

        rc = peer_add(event->connect.conn_handle);
        if (rc != 0) {
            MODLOG_DFLT(ERROR, "Failed to add peer; rc=%d\n", rc);
            break;
        }

        /* Intervalle 20 ms / latence 10 demande juste apres la connexion,
         * avant le bonding, comme Android : sans lui l'aide n'acquitte jamais
         * START. Avec deux aides, les deux liens doivent etre a 20 ms (voir
         * BLE_GAP_EVENT_CONN_UPDATE). */
        {
            struct ble_gap_upd_params conn_params = {
                .itvl_min = 16,   /* 16 * 1.25ms = 20ms */
                .itvl_max = 16,
                .latency = 10,
                .supervision_timeout = 300, /* 3000ms */
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            rc = ble_gap_update_params(event->connect.conn_handle, &conn_params);
            MODLOG_DFLT(INFO, "ASHA[%s]: demande intervalle 20 ms/latence 10, rc=%d\n", ear_name(s), rc);
        }

        /* L'aide exige un lien chiffre/bonde avant d'accepter le CoC audio ;
         * la decouverte demarre sur BLE_GAP_EVENT_ENC_CHANGE. */
        rc = ble_gap_security_initiate(event->connect.conn_handle);
        if (rc != 0) {
            MODLOG_DFLT(ERROR, "Security could not be initiated, rc=%d\n", rc);
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        s = side_by_conn(event->conn_update.conn_handle);
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            MODLOG_DFLT(INFO, "ASHA[%s]: parametres de connexion, status=%d itvl=%d (x1,25 ms)"
                        " latence=%d timeout=%d\n", s ? ear_name(s) : "-", event->conn_update.status,
                        desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        s = side_by_conn(event->enc_change.conn_handle);
        if (s == NULL) {
            break;
        }
        if (event->enc_change.status != 0) {
            MODLOG_DFLT(ERROR, "ASHA[%s]: echec du chiffrement/bonding, status=%d\n",
                        ear_name(s), event->enc_change.status);
            side_fail(s, "chiffrement");
            break;
        }
        MODLOG_DFLT(INFO, "ASHA[%s]: lien chiffre, decouverte du service ASHA\n", ear_name(s));
#if ASHA_LOG_BOND_KEYS
        log_bond_keys(s);
#endif
        /* Decouverte CIBLEE sur le seul service ASHA : une decouverte complete
         * (services proprietaires en plus) prend 30-40 s et l'aide refuse
         * ensuite l'ouverture du CoC (LE COC error 6). */
        rc = peer_disc_svc_by_uuid(event->enc_change.conn_handle, &asha_svc_uuid.u,
                                   blecent_l2cap_coc_on_disc_complete, NULL);
        if (rc != 0) {
            MODLOG_DFLT(ERROR, "Failed to discover services; rc=%d\n", rc);
            side_fail(s, "decouverte de services");
        }
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Ancien bond refuse par l'aide : on l'efface et on recommence. */
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        assert(rc == 0);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        MODLOG_DFLT(INFO, "Repeat pairing: ancien bond efface, nouvel essai\n");
        UNLOCK();
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        MODLOG_DFLT(INFO, "Passkey action requise, type=%d (non gere, Just Works attendu)\n",
                    event->passkey.params.action);
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        /* AudioStatusPoint : 1 octet, 0x00 = succes. Aiguillage par
         * conn_handle : les deux aides ont les memes handles GATT. */
        s = side_by_conn(event->notify_rx.conn_handle);
        if (s == NULL || event->notify_rx.attr_handle != s->h.audio_status_point) {
            break;
        }
        {
            uint8_t status_byte = event->notify_rx.om->om_len >= 1 ? event->notify_rx.om->om_data[0] : 0xFF;
            MODLOG_DFLT(INFO, "ASHA[%s]: AudioStatusPoint status=0x%02x (etat=%d)\n",
                        ear_name(s), status_byte, s->state);
            if (s->state == ASHA_STATE_AWAITING_START_ACK) {
                if (status_byte == 0x00) {
                    MODLOG_DFLT(INFO, "ASHA[%s]: START acquitte, flux audio\n", ear_name(s));
                    s->state = ASHA_STATE_STREAMING;
                    asha_maybe_scan();
                } else {
                    MODLOG_DFLT(ERROR, "ASHA[%s]: START refuse (status=0x%02x)\n", ear_name(s), status_byte);
                    s->state = ASHA_STATE_READY;
                }
            } else if (s->state == ASHA_STATE_AWAITING_STOP_ACK) {
                MODLOG_DFLT(INFO, "ASHA[%s]: STOP acquitte\n", ear_name(s));
                s->state = ASHA_STATE_READY;
                asha_group_update();
            }
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        MODLOG_DFLT(INFO, "disconnect; reason=%d ", event->disconnect.reason);
        print_conn_desc(&event->disconnect.conn);
        MODLOG_DFLT(INFO, "\n");
        peer_delete(event->disconnect.conn.conn_handle);
        s = side_by_conn(event->disconnect.conn.conn_handle);
        if (s != NULL) {
            /* Seule une aide deja annoncee a l'autre (prete ou au-dela) change
             * le groupe ; un echec de configuration ne doit pas couper le son
             * de l'aide en flux. L'etat ne suffit pas : une aide qui tombe
             * pendant la reouverture de son CoC est en SETUP mais faisait
             * partie du groupe (l'autre restait en stereo, 2026-10-11). */
            bool was_in_group = s->in_group;
            MODLOG_DFLT(INFO, "ASHA[%s]: aide deconnectee\n", ear_name(s));
            side_release(s);
            if (was_in_group) {
                asha_on_side_lost();
            }
        }
        /* Scan actif (pas la liste blanche) : retrouve l'aide de facon fiable
         * apres un off/on (teste 2026-10-05). */
        asha_maybe_scan();
        break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        MODLOG_DFLT(INFO, "discovery complete; reason=%d\n", event->disc_complete.reason);
        break;

    default:
        break;
    }
    UNLOCK();
    return 0;
}

static void
blecent_on_reset(int reason)
{
    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason);
}

static void
blecent_on_sync(void)
{
    int rc;

    /* Make sure we have proper identity address set (public preferred) */
    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    LOCK();
    /* Bonds deja en NVS (boot precedent) : connexion "auto-connect" par liste
     * blanche (equivalent Android autoConnect=true), meme apres un RESET de
     * l'ESP32 sans toucher aux aides (teste le 2026-09-24). La premiere aide
     * qui se manifeste est prise ; la seconde est ensuite cherchee par scan.
     * Limitee a 10 s : apres un off/on des aides, la liste blanche ne les
     * retrouvait pas (2026-10-05) ; le scan prend alors le relais (echec de
     * connexion -> asha_maybe_scan). Les bonds appartiennent a la paire principale : rien de tel quand on
     * cible la paire secondaire. */
    ble_addr_t bonded_peers[4];
    int num_bonded = 0;
    rc = ble_store_util_bonded_peers(bonded_peers, &num_bonded,
                                      sizeof(bonded_peers) / sizeof(bonded_peers[0]));
    if (rc == 0 && num_bonded > 0 && ASHA_TARGET_PAIR == 1) {
        rc = ble_gap_wl_set(bonded_peers, num_bonded);
        MODLOG_DFLT(INFO, "ASHA: %d bond(s) existant(s), liste blanche programmee, rc=%d\n",
                    num_bonded, rc);
        uint8_t own_addr_type;
        if (rc == 0 && ble_hs_id_infer_auto(0, &own_addr_type) == 0) {
            int rc_conn = ble_gap_connect(own_addr_type, NULL, 10000,
                                          NULL, blecent_gap_event, NULL);
            MODLOG_DFLT(INFO, "ASHA: auto-connect (liste blanche) arme, rc=%d\n", rc_conn);
            if (rc_conn == 0) {
                g_connecting = true;
                g_connecting_ear = ASHA_EAR_UNKNOWN;
                UNLOCK();
                return;
            }
        }
    }
    blecent_scan();
    UNLOCK();
}

void blecent_host_task(void *param)
{
    ESP_LOGI(tag, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}

static void
temp_log_task(void *arg)
{
    temperature_sensor_handle_t tsens = NULL;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    ESP_ERROR_CHECK(temperature_sensor_install(&cfg, &tsens));
    ESP_ERROR_CHECK(temperature_sensor_enable(tsens));
    while (1) {
        float c = 0.0f;
        if (temperature_sensor_get_celsius(tsens, &c) == ESP_OK) {
            ESP_LOGI("temp", "temperature puce %.1f C", c);
        }
        /* RSSI du lien de chaque aide connectee (mesure du controleur sur les
         * paquets recus de l'aide) : sert aux essais de portee. */
        uint16_t conns[ASHA_SLOT_COUNT];
        int ears[ASHA_SLOT_COUNT];
        int n = 0;
        LOCK();
        for (int i = 0; i < ASHA_SLOT_COUNT; i++) {
            if (g_sides[i].state != ASHA_STATE_FREE) {
                conns[n] = g_sides[i].conn_handle;
                ears[n++] = g_sides[i].ear;
            }
        }
        UNLOCK();
        for (int i = 0; i < n; i++) {
            int8_t rssi;
            if (ble_gap_conn_rssi(conns[i], &rssi) == 0) {
                ESP_LOGI("rssi", "aide %s : %d dBm",
                         ears[i] == ASHA_EAR_LEFT ? "G" : (ears[i] == ASHA_EAR_RIGHT ? "D" : "?"),
                         rssi);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void
app_main(void)
{
    int rc;
    /* Initialize NVS — it is used to store PHY calibration data */
    esp_err_t ret = nvs_flash_init();
    if  (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    g_lock = xSemaphoreCreateRecursiveMutex();
    assert(g_lock != NULL);
    for (int i = 0; i < ASHA_SLOT_COUNT; i++) {
        g_sides[i].tx_unstalled = xSemaphoreCreateBinary();
        assert(g_sides[i].tx_unstalled != NULL);
        side_release(&g_sides[i]);
        xSemaphoreTake(g_sides[i].tx_unstalled, 0);
    }
    const esp_timer_create_args_t timer_args = {
        .callback = asha_restart_timeout,
        .name = "asha_restart",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &g_restart_timer));

    ESP_ERROR_CHECK(audio_capture_start());
    xTaskCreate(temp_log_task, "temp_log", 3072, NULL, 2, NULL);
    xTaskCreate(asha_audio_stream_task, "asha_audio", 4096, NULL, 10, NULL);

    ret = nimble_port_init();
    assert(ret == 0);
    for (int i = 0; i < ASHA_SLOT_COUNT; i++) {
        ble_npl_callout_init(&g_reopen_co[i], nimble_port_get_dflt_eventq(),
                             asha_reopen_cb, &g_sides[i]);
    }

    /* Configure the host. */
    ble_hs_cfg.reset_cb = blecent_on_reset;
    ble_hs_cfg.sync_cb = blecent_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Lien bonde/chiffre exige par l'aide ; ni ecran ni clavier des deux
     * cotes -> appairage "Just Works". */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
#if ASHA_LOG_BOND_KEYS
    /* IRK de l'aide, pour que le sniffer resolve ses adresses privees
     * tournantes (Android demande aussi la distribution de l'identite). */
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ID;
#endif
    ble_hs_cfg.sm_sc = 1;

    blecent_l2cap_coc_mem_init();

    /* Tailles augmentees (exemple : 64/64/64) : la table GATT de l'aide va
     * au moins jusqu'au handle 167 sur ~9 services ; les valeurs par defaut
     * faisaient deborder le pool (status=6 ENOMEM a la decouverte). Le pool
     * est partage par toutes les connexions. */
#if MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
    rc = peer_init(MYNEWT_VAL(BLE_MAX_CONNECTIONS), 20, 8, 120, 60);
    assert(rc == 0);
#else
    rc = peer_init(MYNEWT_VAL(BLE_MAX_CONNECTIONS), 20, 120, 60);
    assert(rc == 0);
#endif
#if CONFIG_BT_NIMBLE_GAP_SERVICE
    /* Set the default device name. */
    rc = ble_svc_gap_device_name_set("blecent-l2coc");
    assert(rc == 0);
#endif

    /* XXX Need to have template for store */
    ble_store_config_init();

    nimble_port_freertos_init(blecent_host_task);
}
