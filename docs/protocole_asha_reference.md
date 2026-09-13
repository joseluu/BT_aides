# Référence protocole ASHA — Signia 2024 (paire de l'utilisateur)

Ce document consolide toutes les données extraites empiriquement (nRF Connect + sniff HCI Android)
sur la paire d'aides auditives Signia visée par le projet. C'est la référence de bas niveau pour
l'implémentation firmware — ne pas re-décoder depuis zéro, ces valeurs sont vérifiées.

Sources :
- Lecture GATT directe via nRF Connect (2026-09-12)
- Sniff HCI Android (`bug_report_HCI/FS/data/log/bt/btsnoop_hci.log`, capturé le 2026-09-12)

## 1. Identité du service ASHA

- Service UUID : **`0xFDF0`** (Bluetooth SIG assigned number, "Hearing Aid" / ASHA)
- Advertising (mode "truncated", hors pairing) : `01 02 30 45 43 45`
  - byte0 = version (0x01), byte1 = capability, bytes2-5 = HiSyncId tronqué (4 octets de poids faible)

## 2. HiSyncId de la paire

```
95:02:44:43:30:45:43:45
```
Identique sur les deux aides (confirmé par lecture ReadOnlyProperties des deux côtés). Sert à
corréler gauche/droite côté host.

## 3. Characteristics du service 0xFDF0

| UUID | Rôle ASHA | Properties |
|---|---|---|
| `6333651e-c481-4a3e-9169-7c902aad37bb` | ReadOnlyProperties | READ |
| `f0d4de7e-4a88-476c-9d9f-1937b0996cc0` | AudioControlPoint | WRITE, WRITE NO RESPONSE |
| `38663f1a-e711-4cac-b641-326b56404837` | AudioStatusPoint | NOTIFY, READ |
| `00e4ca9e-ab14-41e4-8823-f9e70c7e91df` | Volume | WRITE NO RESPONSE |
| `2d410339-82b6-42aa-b34e-e2e01df8cc1a` | LE_PSM_OUT | READ |

**Important** : les *handles* ATT numériques vus dans le sniff (`0x00a0`, `0x00a2`, `0x00a3`,
`0x00a5`, ...) sont propres à cette session GATT — ne pas les coder en dur. Le firmware doit faire
une découverte de service/characteristics par UUID à chaque connexion.

## 4. ReadOnlyProperties — valeurs mesurées

Format (17 octets) :

| Octet(s) | Champ | Valeur (Droite) | Valeur (Gauche) |
|---|---|---|---|
| 0 | Version | `01` | `01` |
| 1 | Device Capabilities | `03` (bit0=1→Right, bit1=1→binaural, bit2=0→pas de CSIS) | `02` (bit0=0→Left, bit1=1→binaural) |
| 2-9 | HiSyncId | `95 02 44 43 30 45 43 45` | idem |
| 10 | Feature Map | `01` (audio streaming LE CoC supporté) | idem |
| 11-12 | Render Delay (LE, ms) | `59 00` → **89 ms** | idem |
| 13-14 | Réservé | `00 00` | idem |
| 15-16 | Codec IDs (LE, bitmask) | `02 00` → bit1 set = **G.722 @ 16 kHz** (seul codec) | idem |

## 5. PSM du canal audio (LE_PSM_OUT)

```
0x0080  (128, décimal)
```
Identique sur les deux aides. C'est le PSM à utiliser pour l'ouverture L2CAP CoC (LE Credit Based
Connection Request).

## 6. Séquence de démarrage observée (par connexion, exécutée deux fois en parallèle — une par
oreillette)

1. `Write Request` sur handle `0x0004` (CCCD Service Changed du service générique) = `02 00`
   — bring-up GATT standard, pas spécifique à ASHA.
2. **L2CAP LE Credit Based Connection Request** — PSM `0x0080`, host propose MTU=512 / MPS=251 /
   credits=65535.
3. L2CAP LE Credit Based Connection Response — résultat `0x0000` (succès), aide impose MTU=248 /
   MPS=247 / credits initiaux=8.
4. `Write Request` sur la CCCD d'AudioStatusPoint = `01 00` (active les notifications).
5. `Write Command` sur Volume = `d8` (int8 signé = **-40 dB**).
6. `Write Request` sur **AudioControlPoint** = `01 01 00 d8 01` → commande **START** :
   - `01` = opcode START
   - `01` = Codec ID (G.722)
   - `00` = Audio Type
   - `d8` = Volume (dupliqué de l'étape 5)
   - `01` = "otherside" state
7. `Handle Value Notification` sur AudioStatusPoint = `00` → ack succès.
8. Le flux audio démarre immédiatement sur le canal L2CAP CoC ouvert à l'étape 2-3.

### Commande STOP

`Write Request` sur AudioControlPoint = `02` (opcode STOP seul, pas d'autre paramètre), suivi d'un
ack `00` sur AudioStatusPoint.

Observé deux fois dans le sniff (deux cycles START/STOP complets sur la même connexion bondée),
ce qui a permis de vérifier le point du §7.

## 7. Format des trames audio (canal L2CAP CoC, PSM 0x0080)

```
octet 0     : 0xa1                       (constant, observé sur 619 trames)
octet 1     : 0x00                       (constant)
octet 2     : numéro de séquence (1 octet, incrémente de 1 par trame de 20 ms, wrap modulo 256)
octets 3-162: 160 octets G.722 encodés   (20 ms audio @ 16 kHz, 64 kbps)
```
Taille totale de la trame (SDU L2CAP) : **163 octets**, inférieure au MPS négocié (247) donc pas de
segmentation nécessaire.

**Comportement du compteur de séquence** : redémarre à la valeur **3** à chaque nouvelle commande
START (vérifié : après un STOP puis un nouveau START sur la même connexion, la séquence repart à 3,
sans continuité avec la session précédente). Ne pas essayer de le faire persister entre deux START.

Les deux oreillettes reçoivent des trames avec le **même numéro de séquence au même instant**, mais
un **contenu audio différent** (encodage indépendant par oreille — pas une simple duplication
d'octets). Pour un relai TV mono, envoyer le même contenu encodé aux deux canaux est acceptable et
plus simple ; ce n'est pas une exigence du protocole que les deux flux soient identiques.

## 8. Flow control L2CAP CoC

Le firmware doit gérer le crédit-based flow control standard du L2CAP CoC : chaque SDU envoyé
consomme un crédit côté émetteur, et le récepteur renvoie périodiquement des trames
`LE Flow Control Credit` (code de signalisation `0x16`) pour réapprovisionner. Observé dans le
sniff à un rythme approximativement toutes les 5-10 trames audio. Le host doit suivre le compteur
de crédits disponibles côté aide et ne pas dépasser le crédit courant côté émission.
