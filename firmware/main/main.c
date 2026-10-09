/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "esp_log.h"
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

#if MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM) >= 1

#define COC_BUF_COUNT          (3 * MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM))
#define L2CAP_COC_UUID         0x1812
#define MTU                    512

/* ==================== Protocole ASHA (Phase 1/2) ====================
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

/* BUG TROUVE (2026-09-13, via le code source de asha_pipewire_sink,
 * asha/AudioPacket.hh) : la vraie trame ASHA au niveau applicatif est
 * simplement {seq (1 octet), data[160]} = 161 octets — PAS de prefixe
 * "0xa1 0x00". Ces deux octets vus dans le sniff HCI sont en realite le
 * champ de longueur SDU du L2CAP CoC, ajoute AUTOMATIQUEMENT par la pile BLE
 * (161 en little-endian = 0xa1 0x00) — pas un en-tete a construire nous-memes.
 * En le prependant nous-memes, NimBLE rajoutait PAR-DESSUS son propre prefixe
 * de longueur (163 => 0xa3 0x00), envoyant une trame corrompue et decalee de
 * 2 octets — ce qui explique le rejet quasi instantane par l'aide (reason=531)
 * observe sur des dizaines d'essais, alors que le serveur de test generique
 * (Round 4) ne validait jamais la structure et acceptait n'importe quoi. */
#define ASHA_FRAME_PAYLOAD_LEN    160
#define ASHA_FRAME_LEN            (1 + ASHA_FRAME_PAYLOAD_LEN) /* 161 : seq + payload */
_Static_assert(ASHA_FRAME_PAYLOAD_LEN == AUDIO_G722_FRAME_BYTES, "trame ASHA = trame G.722 de 20 ms");

struct asha_handles {
    uint16_t read_only_properties;
    uint16_t audio_control_point;
    uint16_t audio_status_point;
    uint16_t audio_status_point_cccd;
    uint16_t volume;
    uint16_t le_psm_out;
};
static struct asha_handles g_asha_handles;
static uint16_t g_asha_psm;
/* 0 dB (volume max, plage ASHA = 0 a -128) — teste au volume max pour
 * confirmation audible ; -40dB (0xd8) etait la valeur du sniff Android. */
static uint8_t g_asha_volume_db = 0x00;
static uint8_t g_asha_seq;

enum asha_state {
    ASHA_STATE_IDLE,
    ASHA_STATE_AWAITING_START_ACK,
    ASHA_STATE_STREAMING,
    ASHA_STATE_AWAITING_STOP_ACK,
};
static enum asha_state g_asha_state = ASHA_STATE_IDLE;

static uint16_t conn_handle_coc ;

/* Adresse d'identite de l'aide, capturee a la premiere connexion reussie
 * (via ble_gap_conn_desc.peer_id_addr — l'adresse d'identite resolue si
 * l'aide utilise une adresse privee resolvable, sinon identique a
 * l'adresse observee sur l'air). Utilisee pour programmer la liste
 * blanche du controleur et permettre une reconnexion "auto-connect" a la
 * maniere d'Android (BluetoothDevice.connectGatt(..., autoConnect=true,
 * ...)) : le controleur radio reste arme en permanence sur cette adresse
 * et se connecte des la moindre annonce, meme tres breve — bien plus
 * fiable qu'un scan actif cote hote pour rattraper une aide qui ne fait
 * plus qu'un advertising peu frequent apres une deconnexion. */
static ble_addr_t g_asha_peer_addr;
static bool g_asha_peer_addr_known = false;
static os_membuf_t sdu_coc_mem[OS_MEMPOOL_SIZE(COC_BUF_COUNT, MTU * 2)];
static struct os_mempool sdu_coc_mbuf_mempool;
static struct os_mbuf_pool sdu_os_mbuf_pool;
static int blecent_l2cap_coc_event_cb(struct ble_l2cap_event *event, void *arg);

struct ble_l2cap_chan *coc_chan = NULL;

/**
 * This API is used to send data over L2CAP connection oriented channel.
 */
static void
blecent_l2cap_coc_send_data(struct ble_l2cap_chan *chan)
{
    struct os_mbuf *sdu_rx_data;
    int rc = 0;
    int len = 512;
    uint8_t value[len];

    for (int i = 0; i < len; i++) {
        value[i] = i;
    }

    do {
        sdu_rx_data = os_mbuf_get_pkthdr(&sdu_os_mbuf_pool, 0);
        if (sdu_rx_data == NULL) {
            vTaskDelay(10 / portTICK_PERIOD_MS);
            sdu_rx_data = os_mbuf_get_pkthdr(&sdu_os_mbuf_pool, 0);
        }
    } while (sdu_rx_data == NULL);

    os_mbuf_append(sdu_rx_data, value, len);

    print_mbuf_data(sdu_rx_data);

    rc = ble_l2cap_send(chan, sdu_rx_data);

    while (rc == BLE_HS_ESTALLED) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        rc = ble_l2cap_send(chan, sdu_rx_data);
    }
    if (rc == 0) {
        MODLOG_DFLT(INFO, "Data sent successfully");
    } else {
        MODLOG_DFLT(INFO, "Data sending failed, rc = %d", rc);
        os_mbuf_free_chain(sdu_rx_data);
    }
}

/**
 * Envoie la commande STOP sur AudioControlPoint (Phase 2 — voir
 * protocole_asha_reference.md §6). Appelee a la fin du flux audio.
 */
static void
asha_send_stop(void)
{
    uint8_t stop_cmd = 0x02;
    int rc;

    g_asha_state = ASHA_STATE_AWAITING_STOP_ACK;
    rc = ble_gattc_write_no_rsp_flat(conn_handle_coc, g_asha_handles.audio_control_point,
                                     &stop_cmd, sizeof(stop_cmd));
    MODLOG_DFLT(INFO, "ASHA: envoi STOP, rc=%d\n", rc);
}

/**
 * Tache d'envoi audio : prend les trames G.722 produites par audio_capture
 * (PCM1808 -> 16 kHz -> G.722) et les envoie sur le canal CoC au format ASHA
 * (voir protocole_asha_reference.md section 7). La cadence est donnee par
 * l'horloge du quartz audio (une trame toutes les 20 ms), pas par un delai.
 * (La tonalite de test G.722 des Phases 2-4 a ete retiree ; elle reste dans
 * l'historique git.)
 */
/* Donne par BLE_L2CAP_EVENT_COC_TX_UNSTALLED : la trame retenue par NimBLE
 * (faute de credits) est partie, on peut envoyer la suivante. */
static SemaphoreHandle_t s_tx_unstalled;

/* coc_chan est remis a NULL par la tache hote NimBLE (COC_DISCONNECTED)
 * pendant que la tache audio s'en sert : sans verrou, ble_l2cap_send(NULL)
 * -> LoadProhibited a l'adresse 0x38 (&chan->coc_tx), observe en direct le
 * 2026-10-09 a la perte de portee. Ce mutex couvre chaque envoi et la remise
 * a NULL ; il n'est jamais tenu pendant une attente. Recursif : au cas ou
 * NimBLE rappellerait COC_DISCONNECTED dans la tache appelante. */
static SemaphoreHandle_t s_coc_chan_mutex;

/* Envoie sdu sur coc_chan s'il existe encore. BLE_HS_ENOTCONN si le canal a
 * disparu (sdu non consomme, a liberer par l'appelant). */
static int
asha_coc_send_locked(struct os_mbuf *sdu)
{
    int rc;

    xSemaphoreTakeRecursive(s_coc_chan_mutex, portMAX_DELAY);
    rc = (coc_chan != NULL) ? ble_l2cap_send(coc_chan, sdu) : BLE_HS_ENOTCONN;
    xSemaphoreGiveRecursive(s_coc_chan_mutex);
    return rc;
}

static void
asha_audio_stream_task(void *arg)
{
    uint8_t frame[ASHA_FRAME_LEN];

    if (s_tx_unstalled == NULL) {
        s_tx_unstalled = xSemaphoreCreateBinary();
    }
    xSemaphoreTake(s_tx_unstalled, 0);
    bool stalled = false;
    unsigned stall_count = 0;

    /* frame = {seq, 160 octets G.722} = 161 octets. Le prefixe de longueur
     * SDU L2CAP est ajoute par NimBLE lui-meme dans ble_l2cap_send() — ne
     * jamais le construire a la main ici (voir explication ci-dessus). */
    /* Le compteur de sequence redemarre a 3 a chaque nouveau START (verifie
     * empiriquement dans le sniff HCI Android — voir reference §7). */
    g_asha_seq = 3;
    audio_capture_restart_stream();
    int underruns = 0;

    /* Nombre de trames consecutives en echec malgre les retentatives — signe
     * que le canal CoC est mort (credits de flux epuises et jamais
     * regeneres par l'aide, observe en test reel : ca ne recupere JAMAIS
     * tout seul, meme apres 45s+ et un retour a portee). Au-dela du seuil,
     * on referme puis rouvre UNIQUEMENT le canal L2CAP CoC (nouveaux
     * credits), sans toucher a la connexion GAP.
     *
     * Important (doc officielle AOSP, source.android.com/docs/core/connect/
     * bluetooth/asha) : "When the central isn't streaming audio data to the
     * peripheral and can maintain a BLE connection, the central shouldn't
     * disconnect from the peripheral." Verifie en test reel : apres une
     * deconnexion GAP complete (essai precedent avec ble_gap_terminate()),
     * l'aide n'emet plus AUCUN advertising avant un cycle d'alimentation
     * manuel — confirme par capture radio (sniffer, 10s, zero paquet).
     * Rouvrir juste le canal CoC evite ce piege : la connexion GAP reste
     * vivante, pas besoin que l'aide re-advertise. */
    int consecutive_failures = 0;
    bool channel_stuck = false;
#define ASHA_MAX_CONSECUTIVE_FRAME_FAILURES 30

    for (int i = 0; coc_chan != NULL; i++) {
        struct os_mbuf *sdu;
        int rc;

        if (stalled) {
            if (xSemaphoreTake(s_tx_unstalled, pdMS_TO_TICKS(1000)) != pdTRUE) {
                MODLOG_DFLT(ERROR, "ASHA: credits CoC non rendus depuis 1 s —"
                            " fermeture/reouverture du canal (connexion GAP conservee)\n");
                channel_stuck = true;
                xSemaphoreTakeRecursive(s_coc_chan_mutex, portMAX_DELAY);
                if (coc_chan != NULL) {
                    ble_l2cap_disconnect(coc_chan);
                }
                xSemaphoreGiveRecursive(s_coc_chan_mutex);
                break;
            }
            if (coc_chan == NULL) {
                break;
            }
            stalled = false;
            if (++stall_count % 50 == 1) {
                MODLOG_DFLT(INFO, "ASHA: attente de credits CoC (%u fois)\n", stall_count);
            }
        }

        if (!audio_capture_get_frame(&frame[1], pdMS_TO_TICKS(100))) {
            if (++underruns % 10 == 1) {
                MODLOG_DFLT(ERROR, "ASHA: pas de trame audio disponible (%d fois)", underruns);
            }
            continue;
        }
        frame[0] = g_asha_seq++;

        do {
            sdu = os_mbuf_get_pkthdr(&sdu_os_mbuf_pool, 0);
            if (sdu == NULL) {
                vTaskDelay(2 / portTICK_PERIOD_MS);
            }
        } while (sdu == NULL);
        os_mbuf_append(sdu, frame, sizeof(frame));

        /* ESTALLED = trame ACCEPTEE : NimBLE la garde et l'enverra quand l'aide
         * rendra des credits (cf. ble_l2cap_coc_continue_tx). Il ne faut ni la
         * renvoyer ni la liberer, seulement attendre TX_UNSTALLED avant la
         * suivante. EBUSY = trame refusee (une autre est encore en cours) : on
         * retente brievement, puis on la libere. */
        rc = asha_coc_send_locked(sdu);
        for (int retry = 0; rc == BLE_HS_EBUSY && retry < 20; retry++) {
            vTaskDelay(5 / portTICK_PERIOD_MS);
            rc = asha_coc_send_locked(sdu);
        }
        if (rc == BLE_HS_ENOTCONN) {
            /* Canal ferme pendant l'envoi (deconnexion) : fin du flux. */
            os_mbuf_free_chain(sdu);
            break;
        }
        if (rc == BLE_HS_ESTALLED) {
            stalled = true;
            consecutive_failures = 0;
        } else if (rc != 0) {
            consecutive_failures++;
            MODLOG_DFLT(ERROR, "ASHA: envoi trame audio echoue, rc=%d (frame %d), trame ignoree"
                        " (%d echecs consecutifs)\n", rc, i, consecutive_failures);
            os_mbuf_free_chain(sdu);
            if (consecutive_failures >= ASHA_MAX_CONSECUTIVE_FRAME_FAILURES) {
                MODLOG_DFLT(ERROR, "ASHA: canal CoC bloque (%d echecs consecutifs) —"
                            " fermeture/reouverture du canal (connexion GAP conservee)\n",
                            consecutive_failures);
                channel_stuck = true;
                xSemaphoreTakeRecursive(s_coc_chan_mutex, portMAX_DELAY);
                if (coc_chan != NULL) {
                    ble_l2cap_disconnect(coc_chan);
                }
                xSemaphoreGiveRecursive(s_coc_chan_mutex);
                break;
            }
        } else {
            consecutive_failures = 0;
        }

    }

    MODLOG_DFLT(INFO, "ASHA: fin du flux audio, envoi STOP\n");
    asha_send_stop();

    if (channel_stuck) {
        /* Laisser BLE_L2CAP_EVENT_COC_DISCONNECTED nettoyer coc_chan/
         * g_asha_state avant de rouvrir — evite une course avec la tache
         * hote NimBLE qui traite l'evenement de facon asynchrone. */
        vTaskDelay(300 / portTICK_PERIOD_MS);
        MODLOG_DFLT(INFO, "ASHA: reouverture du canal CoC (PSM 0x%04x)...\n", g_asha_psm);
        struct os_mbuf *sdu_rx = os_mbuf_get_pkthdr(&sdu_os_mbuf_pool, 0);
        int rc = ble_l2cap_connect(conn_handle_coc, g_asha_psm, MTU, sdu_rx,
                                    blecent_l2cap_coc_event_cb, NULL);
        if (rc != 0) {
            MODLOG_DFLT(ERROR, "ASHA: reouverture du canal CoC echouee, rc=%d"
                        " (connexion GAP probablement perdue entre-temps)\n", rc);
        }
    }

    vTaskDelete(NULL);
}

static int
asha_on_start_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        MODLOG_DFLT(ERROR, "ASHA: envoi START echoue, status=%d\n", error->status);
        g_asha_state = ASHA_STATE_IDLE;
        return 0;
    }
    /* Retour a "attendre l'ack" : un sniff HCI Android frais en session MONO-AIDE
     * (2026-09-12) montre que l'ack sur AudioStatusPoint arrive bel et bien tres
     * vite apres START (2 paquets plus tard) meme sans l'autre oreillette — donc
     * Android l'attend reellement, ce n'est pas juste informatif. Le vrai bug de
     * l'essai precedent (deconnexion a 2.000s) etait `otherstate=0x01` : le
     * meme sniff confirme qu'Android utilise otherstate=0x00 en mono-aide
     * (commande vue : 01 01 00 ec 00). Ce champ est corrige plus haut. */
    MODLOG_DFLT(INFO, "ASHA: START envoye, attente de l'ack via notification AudioStatusPoint...\n");
    return 0;
}

static int
asha_on_cccd_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg)
{
    uint8_t start_cmd[5];
    int rc;

    if (error->status != 0) {
        MODLOG_DFLT(ERROR, "ASHA: activation notifications AudioStatusPoint echouee, status=%d\n",
                    error->status);
        return 0;
    }

    /* Volume est "Write No Response" uniquement (confirme dans le sniff HCI
     * Android d'origine : "Write Command", pas "Write Request") — un
     * ble_gattc_write_flat() classique se fait rejeter par l'aide (observe :
     * status=508). Pas de callback possible ici, donc on enchaine directement
     * sur l'ecriture START apres l'appel. */
    rc = ble_gattc_write_no_rsp_flat(conn_handle_coc, g_asha_handles.volume,
                                     &g_asha_volume_db, sizeof(g_asha_volume_db));
    MODLOG_DFLT(INFO, "ASHA: notifications actives, ecriture Volume (no-rsp) rc=%d\n", rc);

    /* Deuxieme ecriture "mystere" vue dans le sniff mono-aide : `03 00` sur
     * AudioControlPoint, juste apres Volume et juste avant START. Meme
     * remarque que pour le `03 02 10` pre-CoC — reproduite a l'identique. */
    {
        uint8_t mystery_cmd2[2] = {0x03, 0x00};
        int rc2 = ble_gattc_write_no_rsp_flat(conn_handle_coc, g_asha_handles.audio_control_point,
                                              mystery_cmd2, sizeof(mystery_cmd2));
        MODLOG_DFLT(INFO, "ASHA: ecriture pre-START '03 00' sur AudioControlPoint, rc=%d, envoi START...\n",
                    rc2);
    }

    /* Commande START (protocole_asha_reference.md §6) :
     * opcode=0x01 (START), codec=0x01 (G.722), audiotype=0x00, volume, otherstate.
     * Le sniff Android d'origine montrait otherstate=0x01, mais cette capture
     * venait d'une session BINAURALE (les deux aides connectees a la fois) —
     * ici on ne connecte qu'UNE aide pour ce test, donc otherstate=0x00
     * (pas d'autre cote connecte). Hypothese testee suite a une deconnexion
     * (reason=531, "Remote User Terminated") juste apres l'envoi de START avec
     * otherstate=0x01 — l'aide attendait peut-etre sa jumelle. */
    start_cmd[0] = 0x01;
    start_cmd[1] = 0x01;
    start_cmd[2] = 0x00;
    start_cmd[3] = g_asha_volume_db;
    start_cmd[4] = 0x00;

    g_asha_state = ASHA_STATE_AWAITING_START_ACK;
    ble_gattc_write_flat(conn_handle_coc, g_asha_handles.audio_control_point,
                         start_cmd, sizeof(start_cmd), asha_on_start_write, NULL);
    return 0;
}

static int
asha_on_le_psm_out_read(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    uint8_t buf[2];
    int len;
    struct os_mbuf *sdu_rx;

    if (error->status != 0) {
        MODLOG_DFLT(ERROR, "ASHA: lecture LE_PSM_OUT echouee, status=%d\n", error->status);
        return 0;
    }

    len = OS_MBUF_PKTLEN(attr->om);
    if (len < 2) {
        MODLOG_DFLT(ERROR, "ASHA: LE_PSM_OUT trop courte (%d octets)\n", len);
        return 0;
    }
    ble_hs_mbuf_to_flat(attr->om, buf, sizeof(buf), NULL);
    g_asha_psm = buf[0] | (buf[1] << 8);
    MODLOG_DFLT(INFO, "ASHA: PSM lu dynamiquement = 0x%04x\n", g_asha_psm);

    /* Ordre exact observe dans le sniff HCI Android (protocole_asha_reference.md
     * §6) : le canal L2CAP CoC s'ouvre AVANT la sequence CCCD/Volume/START, pas
     * apres. Une premiere tentative en ouvrant apres l'ack de START a ete
     * rejetee par l'aide (LE COC error 6 = Insufficient Authorization). La
     * suite (CCCD/Volume/START) reprend dans asha_start_control_sequence(),
     * declenchee par BLE_L2CAP_EVENT_COC_CONNECTED. */
    MODLOG_DFLT(INFO, "ASHA: ouverture canal L2CAP CoC (PSM 0x%04x)...\n", g_asha_psm);
    sdu_rx = os_mbuf_get_pkthdr(&sdu_os_mbuf_pool, 0);
    ble_l2cap_connect(conn_handle_coc, g_asha_psm, MTU, sdu_rx,
                      blecent_l2cap_coc_event_cb, NULL);
    return 0;
}

/**
 * Appelee une fois le canal L2CAP CoC reellement ouvert (BLE_L2CAP_EVENT_COC_CONNECTED).
 * Lance la suite de la sequence de controle : CCCD -> Volume -> START.
 */
static void
asha_start_control_sequence(void)
{
    uint16_t cccd_val = 1; /* active les notifications */

    MODLOG_DFLT(INFO, "ASHA: canal CoC ouvert, activation notifications AudioStatusPoint...\n");
    ble_gattc_write_flat(conn_handle_coc, g_asha_handles.audio_status_point_cccd,
                         &cccd_val, sizeof(cccd_val), asha_on_cccd_write, NULL);
}

static int
asha_on_readonly_props_read(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    uint8_t buf[17];
    int len;

    if (error->status != 0) {
        MODLOG_DFLT(ERROR, "ASHA: lecture ReadOnlyProperties echouee, status=%d\n", error->status);
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
        MODLOG_DFLT(INFO, "ASHA ReadOnlyProperties: version=%d capability=0x%02x "
                    "hisyncid=%016llx codecs=0x%04x\n",
                    buf[0], buf[1], (unsigned long long)hisyncid, codecs);
    } else {
        MODLOG_DFLT(ERROR, "ASHA: ReadOnlyProperties trop courte (%d octets)\n", len);
    }

    MODLOG_DFLT(INFO, "ASHA: lecture LE_PSM_OUT...\n");
    ble_gattc_read(conn_handle_coc, g_asha_handles.le_psm_out, asha_on_le_psm_out_read, NULL);
    return 0;
}

/**
 * After connection is established on GAP layer, service discovery is performed. On
 * it's completion, this API locate les characteristics ASHA par UUID et lance la
 * sequence protocolaire reelle (lecture ReadOnlyProperties/PSM, START, puis
 * ouverture du canal L2CAP une fois l'ack recu — voir asha_on_le_psm_out_read
 * et le cas BLE_GAP_EVENT_NOTIFY_RX dans blecent_gap_event).
 */
static void
blecent_l2cap_coc_on_disc_complete(const struct peer *peer, int status, void *arg)
{
    const struct peer_chr *chr;
    const struct peer_dsc *dsc;

    MODLOG_DFLT(INFO, "Decouverte de services terminee, status=%d\n", status);

    chr = peer_chr_find_uuid(peer, &asha_svc_uuid.u, &asha_readonly_props_uuid.u);
    if (chr == NULL) {
        MODLOG_DFLT(ERROR, "ASHA: characteristic ReadOnlyProperties introuvable\n");
        return;
    }
    g_asha_handles.read_only_properties = chr->chr.val_handle;

    chr = peer_chr_find_uuid(peer, &asha_svc_uuid.u, &asha_audio_control_point_uuid.u);
    if (chr == NULL) {
        MODLOG_DFLT(ERROR, "ASHA: characteristic AudioControlPoint introuvable\n");
        return;
    }
    g_asha_handles.audio_control_point = chr->chr.val_handle;

    chr = peer_chr_find_uuid(peer, &asha_svc_uuid.u, &asha_audio_status_point_uuid.u);
    if (chr == NULL) {
        MODLOG_DFLT(ERROR, "ASHA: characteristic AudioStatusPoint introuvable\n");
        return;
    }
    g_asha_handles.audio_status_point = chr->chr.val_handle;

    dsc = peer_dsc_find_uuid(peer, &asha_svc_uuid.u, &asha_audio_status_point_uuid.u,
                             &asha_cccd_uuid.u);
    if (dsc == NULL) {
        MODLOG_DFLT(ERROR, "ASHA: descripteur CCCD d'AudioStatusPoint introuvable\n");
        return;
    }
    g_asha_handles.audio_status_point_cccd = dsc->dsc.handle;

    chr = peer_chr_find_uuid(peer, &asha_svc_uuid.u, &asha_volume_uuid.u);
    if (chr == NULL) {
        MODLOG_DFLT(ERROR, "ASHA: characteristic Volume introuvable\n");
        return;
    }
    g_asha_handles.volume = chr->chr.val_handle;

    chr = peer_chr_find_uuid(peer, &asha_svc_uuid.u, &asha_le_psm_out_uuid.u);
    if (chr == NULL) {
        MODLOG_DFLT(ERROR, "ASHA: characteristic LE_PSM_OUT introuvable\n");
        return;
    }
    g_asha_handles.le_psm_out = chr->chr.val_handle;

    /* Ecriture `03 02 10` (vue avant l'ouverture CoC dans le sniff Android)
     * TESTEE et RETIREE : elle a fait ressurgir "LE COC error: 6" (Insufficient
     * Authorization) qui n'apparaissait plus sans elle. Cause exacte inconnue —
     * possiblement un opcode qui met l'aide dans un etat inattendu vu hors de
     * son contexte Android complet. Ne pas la reintroduire sans comprendre
     * pourquoi elle casse le CoC. Isolee de `03 00` (testee separement plus
     * bas, avant START) pour ne pas re-brouiller les deux hypotheses. */

    MODLOG_DFLT(INFO, "ASHA: toutes les characteristics trouvees, lecture ReadOnlyProperties...\n");
    ble_gattc_read(conn_handle_coc, g_asha_handles.read_only_properties,
                  asha_on_readonly_props_read, NULL);
}

/**
 * The nimble host executes this callback when a L2CAP  event occurs.  The
 * application associates a L2CAP event callback with each connection that is
 * established.  blecent_l2cap_coc uses the same callback for all connections.
 *
 * @param event                 The event being signalled.
 * @param arg                   Application-specified argument; unused by
 *                                  blecent_l2cap_coc.
 *
 * @return                      0 if the application successfully handled the
 *                                  event; nonzero on failure.  The semantics
 *                                  of the return code is specific to the
 *                                  particular L2CAP event being signalled.
 */
static int
blecent_l2cap_coc_event_cb(struct ble_l2cap_event *event, void *arg)
{
    struct ble_l2cap_chan_info chan_info;

    switch (event->type) {
    case BLE_L2CAP_EVENT_COC_CONNECTED:
        if (event->connect.status) {
            console_printf("LE COC error: %d\n", event->connect.status);
            return 0;
        }

        if (ble_l2cap_get_chan_info(event->connect.chan, &chan_info)) {
            assert(0);
        }

        console_printf("LE COC connected, conn: %d, chan: %p, psm: 0x%02x,"
                       " scid: 0x%04x, ""dcid: 0x%04x, our_mps: %d, our_mtu: %d,"
                       " peer_mps: %d, peer_mtu: %d\n",
                       event->connect.conn_handle, event->connect.chan,
                       chan_info.psm, chan_info.scid, chan_info.dcid,
                       chan_info.our_l2cap_mtu, chan_info.our_coc_mtu,
                       chan_info.peer_l2cap_mtu, chan_info.peer_coc_mtu);

        coc_chan = event->connect.chan;

        /* Canal ouvert AVANT la sequence CCCD/Volume/START (ordre exact du
         * sniff Android — voir asha_on_le_psm_out_read). On enchaine ici. */
        asha_start_control_sequence();
        return 0;

    case BLE_L2CAP_EVENT_COC_DISCONNECTED:
        console_printf("LE CoC disconnected, chan: %p\n",
                       event->disconnect.chan);
        xSemaphoreTakeRecursive(s_coc_chan_mutex, portMAX_DELAY);
        coc_chan = NULL;
        xSemaphoreGiveRecursive(s_coc_chan_mutex);
        g_asha_state = ASHA_STATE_IDLE;
        if (s_tx_unstalled != NULL) {
            xSemaphoreGive(s_tx_unstalled);
        }
        return 0;

    case BLE_L2CAP_EVENT_COC_TX_UNSTALLED:
        if (s_tx_unstalled != NULL) {
            xSemaphoreGive(s_tx_unstalled);
        }
        return 0;

    default:
        return 0;
    }
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
#endif // #if  MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM) >= 1

/**
 * Called when service discovery of the specified peer has completed.
 */
#if MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM) < 1
static void
blecent_on_disc_complete(const struct peer *peer, int status, void *arg)
{

    if (status != 0) {
        /* Service discovery failed.  Terminate the connection. */
        MODLOG_DFLT(ERROR, "Error: Service discovery failed; status=%d "
                    "conn_handle=%d\n", status, peer->conn_handle);
        ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    /* Service discovery has completed successfully.  Now we have a complete
     * list of services, characteristics, and descriptors that the peer
     * supports.
     */
    MODLOG_DFLT(INFO, "Service discovery complete; status=%d "
                "conn_handle=%d\n", status, peer->conn_handle);

    /* Now user can perform GATT procedures against the peer: read,
     * write, and subscribe to notifications.
     */
}
#endif

/**
 * Initiates the GAP general discovery procedure.
 */
static void
blecent_scan(void)
{
    uint8_t own_addr_type;
    struct ble_gap_disc_params disc_params = {0};
    int rc;

    /* Figure out address to use while advertising (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
        return;
    }

    /* Duplicates NON filtres : le controleur risque de supprimer le SCAN_RSP
     * (qui porte les donnees ASHA) s'il a deja vu l'ADV_IND de la meme adresse,
     * ce qui nous ferait rater la detection en filtrant. On accepte le bruit
     * en plus pour ce test de diagnostic.
     */
    disc_params.filter_duplicates = 0;

    /**
     * Scan actif (hypothese testee suite a un premier essai en passif sans
     * resultat) : si le service ASHA (0xFDF0) est annonce dans le paquet de
     * reponse de scan (SCAN_RSP) plutot que dans l'ADV_IND primaire, un scan
     * passif ne le verrait jamais — l'actif recupere les deux.
     */
    disc_params.passive = 0;

    /* Use defaults for the rest of the parameters. */
    disc_params.itvl = 0;
    disc_params.window = 0;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params,
                      blecent_gap_event, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Error initiating GAP discovery procedure; rc=%d\n",
                    rc);
    }
}

/**
 * Indicates whether we should try to connect to the sender of the specified
 * advertisement.  The function returns a positive result if the device
 * advertises connectability and support for the Alert Notification service.
 */
#if CONFIG_EXAMPLE_EXTENDED_ADV
static int
ext_blecent_should_connect(const struct ble_gap_ext_disc_desc *disc)
{
    int offset = 0;
    int ad_struct_len = 0;
    uint8_t test_addr[6];
    uint32_t peer_addr[6];

    memset(peer_addr, 0x0, sizeof peer_addr);

    if (disc->legacy_event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
            disc->legacy_event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) {
        return 0;
    }
    if (strlen(CONFIG_EXAMPLE_PEER_ADDR) &&
            (strncmp(CONFIG_EXAMPLE_PEER_ADDR, "ADDR_ANY", strlen("ADDR_ANY")) != 0)) {
        ESP_LOGI(tag, "Peer address from menuconfig: %s", CONFIG_EXAMPLE_PEER_ADDR);
        /* Convert string to address */
        sscanf(CONFIG_EXAMPLE_PEER_ADDR, "%lx:%lx:%lx:%lx:%lx:%lx",
               &peer_addr[5], &peer_addr[4], &peer_addr[3],
               &peer_addr[2], &peer_addr[1], &peer_addr[0]);

        /* Conversion */
        for (int i=0; i<6; i++) {
            test_addr[i] = (uint8_t )peer_addr[i];
        }

        if (memcmp(test_addr, disc->addr.val, sizeof(disc->addr.val)) != 0) {
            return 0;
        }
    }
    /* The device has to advertise support for L2CAP COC UUID (0x1812).
    */
    do {
        ad_struct_len = disc->data[offset];

        if (!ad_struct_len) {
            break;
        }

        /* Search if ANS UUID is advertised */
        if (disc->data[offset] == 0x03 && disc->data[offset + 1] == 0x03) {
            if ( disc->data[offset + 2] == 0x18 && disc->data[offset + 3] == 0x12 ) {
                return 1;
            }
        }

        offset += ad_struct_len + 1;

    } while ( offset < disc->length_data );
    return 0;
}
#else
/* HiSyncId tronque (4 octets de poids faible) de la paire Signia ciblee — voir
 * docs/protocole_asha_reference.md paragraphe 2 et 6. Sert a ne se connecter qu'a
 * l'une de nos deux aides, pas a un autre appareil ASHA qui traînerait a proximite. */
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

static int
blecent_should_connect(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    int rc;

    /* On accepte aussi SCAN_RSP : le service data 0xFDF0 peut arriver dans la
     * reponse de scan plutot que dans l'ADV_IND primaire (a confirmer). */
    if (disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
            disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND &&
            disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {

        return 0;
    }

    rc = ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data);
    if (rc != 0) {
        return 0;
    }

    /* On cible le service ASHA (0xFDF0), annonce en "Service Data - 16-bit UUID".
     * svc_data_uuid16 = [uuid_lo, uuid_hi, version, capability, hisyncid_tronque x4] */
    if (fields.svc_data_uuid16 == NULL || fields.svc_data_uuid16_len < 8) {
        return 0;
    }
    if (fields.svc_data_uuid16[0] != 0xF0 || fields.svc_data_uuid16[1] != 0xFD) {
        return 0;
    }

    /* Log toujours visible (INFO) des octets bruts, pour diagnostic — meme si le
     * HiSyncId ne correspond pas ensuite. */
    MODLOG_DFLT(INFO, "Service 0xFDF0 vu, event_type=%d, data brute (%d octets): ",
                disc->event_type, fields.svc_data_uuid16_len);
    for (int k = 0; k < fields.svc_data_uuid16_len; k++) {
        MODLOG_DFLT(INFO, "%02x ", fields.svc_data_uuid16[k]);
    }
    MODLOG_DFLT(INFO, "\n");

    if (memcmp(&fields.svc_data_uuid16[4], g_asha_expected_truncated_hisyncid, 4) != 0) {
        MODLOG_DFLT(INFO, "Appareil ASHA vu mais HiSyncId ne correspond pas, ignore\n");
        return 0;
    }

    MODLOG_DFLT(INFO, "Aide auditive ciblee trouvee (service 0xFDF0, HiSyncId ok)\n");
    return 1;
}
#endif

/**
 * Connects to the sender of the specified advertisement if it looks
 * interesting.  A device is "interesting" if it advertises connectability and
 * support for the Alert Notification service.
 */
static void
blecent_connect_if_interesting(void *disc)
{
    uint8_t own_addr_type;
    int rc;
    ble_addr_t *addr;

    /* Don't do anything if we don't care about this advertiser. */
#if CONFIG_EXAMPLE_EXTENDED_ADV
    if (!ext_blecent_should_connect((struct ble_gap_ext_disc_desc *)disc)) {
        return;
    }
#else
    if (!blecent_should_connect((struct ble_gap_disc_desc *)disc)) {
        return;
    }
#endif

    MODLOG_DFLT(INFO, "Aide auditive ciblee, tentative de connexion...\n");

#if !(MYNEWT_VAL(BLE_HOST_ALLOW_CONNECT_WITH_SCAN))
    /* Scanning must be stopped before a connection can be initiated. */
    rc = ble_gap_disc_cancel();
    if (rc != 0) {
        MODLOG_DFLT(DEBUG, "Failed to cancel scan; rc=%d\n", rc);
        return;
    }
#endif

    /* Figure out address to use for connect (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
        return;
    }

    /* Try to connect the the advertiser.  Allow 30 seconds (30000 ms) for
     * timeout.
     */
#if CONFIG_EXAMPLE_EXTENDED_ADV
    addr = &((struct ble_gap_ext_disc_desc *)disc)->addr;
#else
    addr = &((struct ble_gap_disc_desc *)disc)->addr;
#endif

    rc = ble_gap_connect(own_addr_type, addr, 30000, NULL,
                         blecent_gap_event, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Error: Failed to connect to device; addr_type=%d "
                    "addr=%s; rc=%d\n",
                    addr->type, addr_str(addr->val), rc);
        return;
    }
}

/**
 * The nimble host executes this callback when a GAP event occurs.  The
 * application associates a GAP event callback with each connection that is
 * established.  blecent uses the same callback for all connections.
 *
 * @param event                 The event being signalled.
 * @param arg                   Application-specified argument; unused by
 *                              blecent.
 *
 * @return                      0 if the application successfully handled the
 *                              event; nonzero on failure.  The semantics
 *                              of the return code is specific to the
 *                              particular GAP event being signalled.
 */
static int
blecent_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    struct ble_hs_adv_fields fields;
    int rc;
    peer_disc_fn *disc_cb = NULL;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        /* Compteur/log toujours visible (INFO) — diagnostic pour verifier que le
         * scan recoit bien des paquets, independamment du filtrage ASHA plus bas. */
        {
            static uint32_t disc_count = 0;
            disc_count++;
            if (disc_count % 1000 == 1) {
                MODLOG_DFLT(INFO, "Scan: %lu paquets recus (dernier addr=%s, "
                            "event_type=%d, rssi=%d)\n",
                            (unsigned long)disc_count, addr_str(event->disc.addr.val),
                            event->disc.event_type, event->disc.rssi);
            }
        }

        rc = ble_hs_adv_parse_fields(&fields, event->disc.data,
                                     event->disc.length_data);
        if (rc != 0) {
            return 0;
        }

        /* An advertisement report was received during GAP discovery. */
        print_adv_fields(&fields);

        /* Try to connect to the advertiser if it looks interesting. */
        blecent_connect_if_interesting(&event->disc);
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        /* A new connection was established or a connection attempt failed. */
        if (event->connect.status == 0) {
            /* Connection successfully established. */
            MODLOG_DFLT(INFO, "Connection established ");

            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            assert(rc == 0);
            print_conn_desc(&desc);
            MODLOG_DFLT(INFO, "\n");

            conn_handle_coc = event->connect.conn_handle;

            /* Remember peer. */
            rc = peer_add(event->connect.conn_handle);
            if (rc != 0) {
                MODLOG_DFLT(ERROR, "Failed to add peer; rc=%d\n", rc);
                return 0;
            }

            /* Capture l'adresse d'identite pour la liste blanche (voir
             * commentaire pres de g_asha_peer_addr). peer_id_addr (pas
             * peer_ota_addr) car c'est l'adresse resolue/stable si l'aide
             * utilise une adresse privee resolvable. */
            g_asha_peer_addr = desc.peer_id_addr;
            g_asha_peer_addr_known = true;
            rc = ble_gap_wl_set(&g_asha_peer_addr, 1);
            MODLOG_DFLT(INFO, "ASHA: liste blanche programmee (%s), rc=%d\n",
                        addr_str(g_asha_peer_addr.val), rc);

            /* L'aide auditive exige un lien chiffre/bonde avant d'accepter le
             * canal L2CAP CoC audio. On initie l'appairage ici ; la decouverte
             * de services (et l'ouverture du canal CoC) n'a lieu qu'une fois
             * BLE_GAP_EVENT_ENC_CHANGE recu avec succes, plus bas. */

            /* Renegociation de l'intervalle de connexion — vue dans un sniff
             * HCI Android frais (session mono-aide, 2026-09-12) : Android
             * demande explicitement 20ms/20ms, latency=10, timeout=3000ms
             * juste apres la connexion, AVANT le bonding/decouverte. Notre
             * intervalle par defaut (30-50ms, latency=0) est probablement trop
             * lent pour la cadence audio 20ms — hypothese retenue pour
             * expliquer pourquoi l'aide n'envoie jamais l'ack de START (elle
             * attend peut-etre un lien deja renegocie pour du streaming avant
             * d'accepter). A verifier au prochain test.
             */
            {
                struct ble_gap_upd_params conn_params = {
                    .itvl_min = 16,   /* 16 * 1.25ms = 20ms */
                    .itvl_max = 16,
                    .latency = 10,
                    .supervision_timeout = 300, /* 300 * 10ms = 3000ms */
                    .min_ce_len = 0,
                    .max_ce_len = 0,
                };
                int rc_upd = ble_gap_update_params(event->connect.conn_handle, &conn_params);
                MODLOG_DFLT(INFO, "Renegociation intervalle connexion (20ms/latency=10) rc=%d\n",
                            rc_upd);
            }

            rc = ble_gap_security_initiate(event->connect.conn_handle);
            if (rc != 0) {
                MODLOG_DFLT(ERROR, "Security could not be initiated, rc=%d\n", rc);
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            } else {
                MODLOG_DFLT(INFO, "Appairage/bonding initie...\n");
            }
        } else {
            /* Connection attempt failed; resume scanning. */
            MODLOG_DFLT(ERROR, "Error: Connection failed; status=%d\n",
                        event->connect.status);
            blecent_scan();
        }

        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        /* Le lien vient d'etre chiffre (bonding termine) ou l'operation a echoue. */
        MODLOG_DFLT(INFO, "Encryption change event; status=%d ",
                    event->enc_change.status);
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        assert(rc == 0);
        print_conn_desc(&desc);
        MODLOG_DFLT(INFO, "\n");

        if (event->enc_change.status != 0) {
            MODLOG_DFLT(ERROR, "Echec du chiffrement/bonding, abandon\n");
            return 0;
        }

        MODLOG_DFLT(INFO, "Lien chiffre — lancement de la decouverte ciblee du service ASHA\n");
#if MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM) >= 1
        disc_cb = blecent_l2cap_coc_on_disc_complete;
#else
        disc_cb = blecent_on_disc_complete;
#endif
        /* Decouverte CIBLEE sur le seul service ASHA (0xFDF0), pas peer_disc_all()
         * sur toute la table GATT. Cause identifiee : l'aide a plusieurs services
         * proprietaires Signia en plus d'ASHA ; une decouverte complete prend
         * 30-40s, et l'aide semble n'autoriser l'ouverture du canal L2CAP CoC
         * que dans une fenetre de temps limitee apres la connexion — au-dela,
         * elle la refuse (LE COC error 6 = Insufficient Authorization), alors
         * qu'une ouverture rapide (~8s, decouverte complete mais tronquee par
         * ENOMEM avec l'ancienne petite pool) avait fonctionne en Phase 0. Un
         * vrai client Android ne re-decouvre certainement pas les services
         * propriétaires du fabricant a chaque reconnexion non plus. */
        rc = peer_disc_svc_by_uuid(event->enc_change.conn_handle, &asha_svc_uuid.u,
                                   disc_cb, NULL);
        if (rc != 0) {
            MODLOG_DFLT(ERROR, "Failed to discover services; rc=%d\n", rc);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Un bond existait deja avec ce peer (essai precedent) mais il tente un
         * nouvel appairage — on jette l'ancien bond et on relance proprement. */
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        assert(rc == 0);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        MODLOG_DFLT(INFO, "Repeat pairing: ancien bond efface, nouvel essai\n");
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        /* Non attendu en usage normal (aide auditive = Just Works, pas d'IO) ;
         * logge seulement pour visibilite si jamais ca se produit. */
        MODLOG_DFLT(INFO, "Passkey action requise, type=%d (non gere, Just Works attendu)\n",
                    event->passkey.params.action);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        /* Ack d'AudioStatusPoint (voir protocole_asha_reference.md §6) — un seul
         * octet, 0x00 = succes. On distingue ack de START vs ack de STOP via
         * g_asha_state pour ne pas rouvrir le canal CoC sur l'ack du STOP. */
        if (event->notify_rx.attr_handle == g_asha_handles.audio_status_point) {
            uint8_t status_byte = 0xFF;
            if (event->notify_rx.om->om_len >= 1) {
                status_byte = event->notify_rx.om->om_data[0];
            }
            MODLOG_DFLT(INFO, "ASHA: notification AudioStatusPoint, status=0x%02x (etat=%d)\n",
                        status_byte, g_asha_state);

            if (g_asha_state == ASHA_STATE_AWAITING_START_ACK) {
                if (status_byte == 0x00) {
                    /* Le canal CoC est deja ouvert (ouvert avant START, voir
                     * asha_on_le_psm_out_read) — on demarre juste l'envoi des
                     * trames de test maintenant que l'aide a acquitte le START. */
                    MODLOG_DFLT(INFO, "ASHA: START acquitte, demarrage du test audio\n");
                    g_asha_state = ASHA_STATE_STREAMING;
                    xTaskCreate(asha_audio_stream_task, "asha_audio", 4096, NULL, 10, NULL);
                } else {
                    MODLOG_DFLT(ERROR, "ASHA: START refuse par l'aide (status=0x%02x)\n", status_byte);
                    g_asha_state = ASHA_STATE_IDLE;
                }
            } else if (g_asha_state == ASHA_STATE_AWAITING_STOP_ACK) {
                MODLOG_DFLT(INFO, "ASHA: STOP acquitte\n");
                g_asha_state = ASHA_STATE_IDLE;
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        /* Connection terminated. */
        MODLOG_DFLT(INFO, "disconnect; reason=%d ", event->disconnect.reason);
        print_conn_desc(&event->disconnect.conn);
        MODLOG_DFLT(INFO, "\n");

        /* Forget about peer. */
        peer_delete(event->disconnect.conn.conn_handle);

        /* La reconnexion par liste blanche (ble_gap_connect peer_addr=NULL)
         * ne retrouvait pas l'aide apres un off/on (teste 2026-10-05) ; le
         * scan actif, lui, la detecte de facon fiable via le service ASHA. */
        blecent_scan();
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        MODLOG_DFLT(INFO, "discovery complete; reason=%d\n",
                    event->disc_complete.reason);
        return 0;

#if CONFIG_EXAMPLE_EXTENDED_ADV
    case BLE_GAP_EVENT_EXT_DISC:
        /* An advertisement report was received during GAP discovery. */
        ext_print_adv_report(&event->ext_disc);

        blecent_connect_if_interesting(&event->ext_disc);
        return 0;
#endif

    default:
        return 0;
    }
}

void
ble_coc_cent_task(void *pvParameters)
{
    while (1) {
        if (coc_chan) {
            for (int i = 0; i < 5; i++) {
                blecent_l2cap_coc_send_data(coc_chan);
                vTaskDelay(500 / portTICK_PERIOD_MS);
            }
            coc_chan = NULL;
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
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

    /* Si l'aide a deja ete appairee lors d'un boot precedent (bond persiste
     * en NVS), tenter directement une reconnexion "auto-connect" par liste
     * blanche (equivalent Android autoConnect=true) au lieu de repartir sur
     * un scan actif — meme apres un RESET de l'ESP32, sans rien toucher a
     * l'aide. C'est le scenario teste en direct le 2026-09-24. Si aucun bond
     * n'existe (tout premier demarrage), on retombe sur le scan classique. */
    ble_addr_t bonded_peers[4];
    int num_bonded = 0;
    rc = ble_store_util_bonded_peers(bonded_peers, &num_bonded,
                                      sizeof(bonded_peers) / sizeof(bonded_peers[0]));
    /* Les bonds en NVS appartiennent a la paire principale : pas d'auto-connect
     * au demarrage quand on cible la paire secondaire (scan + filtre HiSyncId
     * a la place). Les bonds sont conserves, pas effaces. */
    if (rc == 0 && num_bonded > 0 && ASHA_TARGET_PAIR == 1) {
        g_asha_peer_addr = bonded_peers[0];
        g_asha_peer_addr_known = true;
        rc = ble_gap_wl_set(&g_asha_peer_addr, 1);
        MODLOG_DFLT(INFO, "ASHA: bond existant trouve (%s), liste blanche"
                    " programmee, rc=%d\n", addr_str(g_asha_peer_addr.val), rc);

        uint8_t own_addr_type;
        int rc_addr = ble_hs_id_infer_auto(0, &own_addr_type);
        if (rc_addr == 0) {
            int rc_conn = ble_gap_connect(own_addr_type, NULL, BLE_HS_FOREVER,
                                          NULL, blecent_gap_event, NULL);
            MODLOG_DFLT(INFO, "ASHA: auto-connect (liste blanche) arme au"
                        " demarrage, rc=%d\n", rc_conn);
            if (rc_conn == 0) {
                return;
            }
        }
        /* Echec du demarrage de l'auto-connect — repli sur le scan. */
    }

    /* Begin scanning for a peripheral to connect to. */
    blecent_scan();
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

    ESP_ERROR_CHECK(audio_capture_start());
    xTaskCreate(temp_log_task, "temp_log", 3072, NULL, 2, NULL);

    s_coc_chan_mutex = xSemaphoreCreateRecursiveMutex();
    assert(s_coc_chan_mutex != NULL);

    ret = nimble_port_init();
    assert(ret == 0);

    /* Configure the host. */
    ble_hs_cfg.reset_cb = blecent_on_reset;
    ble_hs_cfg.sync_cb = blecent_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* L'aide auditive exige un lien bonde/chiffre (voir protocole_asha_reference.md).
     * Pas d'ecran/clavier ni cote ESP32 ni cote aide -> pairing "Just Works". */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_sc = 1;

#if MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM) >= 1
    blecent_l2cap_coc_mem_init();
    /* La tache de demo d'origine (ble_coc_cent_task) envoie des paquets de test
     * de 512 octets sans tenir compte du peer_mtu negocie — contre notre aide
     * reelle (peer_mtu=248), ca echoue et semble provoquer la fermeture du
     * canal/lien cote aide. Desactivee : pour ce test de connectivite, on veut
     * juste verifier que le canal CoC s'ouvre et reste stable, sans y envoyer
     * de donnees. Le vrai envoi (trames G.722/ASHA) viendra en Phase 2.
     */
    /* xTaskCreate(ble_coc_cent_task, "ble_coc_cent_task", 4096, NULL, 10, NULL); */
#endif

    /* Initialize data structures to track connected peers.
     * Tailles augmentees (etaient a 64/64/64) : la table GATT reelle de l'aide
     * Signia va au moins jusqu'au handle 167 sur ~9 services (plusieurs
     * proprietaires Signia avant le service ASHA) — les valeurs par defaut de
     * l'exemple faisaient deborder le pool (observe : "Service discovery
     * status=6" = ENOMEM, puis des characteristics ASHA introuvables ensuite
     * car jamais enregistrees faute de place). */
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
