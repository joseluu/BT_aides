# Architecture — Relais audio TV vers aides auditives (host ASHA sur ESP32)

## 1. Objectif

Construire un boîtier autonome branché sur la sortie audio analogique d'une télévision, qui joue
le rôle normalement tenu par un téléphone Android : **host/central ASHA**, diffusant l'audio en
direct vers une paire d'aides auditives Signia (2024) déjà appairées avec Android/iPhone.

Contrainte fondamentale : ASHA n'est pas un profil audio Bluetooth classique. C'est un service GATT
custom (`0xFDF0`) + un canal **L2CAP Connection-Oriented Channel** transportant de l'audio G.722
encodé. Le détail complet du protocole mesuré est dans
[`protocole_asha_reference.md`](./protocole_asha_reference.md) — ce document-ci ne le reprend pas,
il décrit comment le construire.

## 2. Choix matériel

### 2.1 MCU

**ESP32-S3** (recommandé) plutôt qu'un ESP32 classique : plus de RAM/PSRAM pour le pipeline audio
(buffers I2S + encodeur G.722 + 2 connexions BLE simultanées avec leurs canaux CoC), et marge CPU
pour tenir la cadence temps réel de 20 ms sans jitter. Aucun besoin de LE Audio/Auracast (BLE
5.2+/ISO channels) — ASHA fonctionne en BLE 4.2/5.0 classique (GATT + L2CAP CoC), donc pas besoin
d'un ESP32-C6/H2.

Pile BLE : **NimBLE** (intégrée à ESP-IDF) plutôt que Bluedroid — empreinte mémoire plus faible,
API L2CAP CoC disponible côté client comme côté serveur. *À vérifier en Phase 0* : le comportement
exact de l'API client CoC (`ble_l2cap_coc_connect` ou équivalent) sur la version d'ESP-IDF utilisée,
avant de figer le design autour.

### 2.2 Entrée audio (TV analogique → numérique)

La TV fournit un signal analogique (jack ligne, ou sortie casque). Il faut le numériser proprement
en 16 kHz mono avant l'encodage G.722 :

- Codec/ADC I2S externe recommandé (ex. PCM1808, ou tout breakout ADC stéréo I2S bon marché) plutôt
  que l'ADC interne de l'ESP32 (bruit de quantification et couplage numérique trop importants pour
  une captation ligne propre).
- Le module ADC choisi tournera probablement nativement en 44.1/48 kHz stéréo — il faudra downmixer
  en mono puis rééchantillonner vers 16 kHz avant G.722 (voir §3.4).
- Prévoir un étage d'atténuation/adaptation de niveau en amont si la sortie TV dépasse le niveau
  ligne attendu par l'ADC (protection + éviter l'écrêtage).

### 2.3 Alimentation

Boîtier fixe à côté de la TV → alimentation secteur (adaptateur USB 5V), pas de contrainte batterie.
Simplifie la conception (pas de gestion d'énergie critique).

## 3. Architecture logicielle (firmware ESP32)

```
 [TV analogique] --> [ADC I2S] --> [Capture I2S] --> [Downmix + Resample 16kHz mono]
                                                              |
                                                              v
                                                     [Encodeur G.722]
                                                              |
                                                              v
                                                     [Framer ASHA + Séquenceur]
                                                        /              \
                                                       v                v
                                          [L2CAP CoC — Aide Gauche]  [L2CAP CoC — Aide Droite]
                                                       ^                ^
                                                       |                |
                                          [GATT Client / Session ASHA — par aide]
                                                       |                |
                                          [BLE Central Manager — scan/connect/bond]
```

### 3.1 BLE Central Manager

- Scan des advertisements `0xFDF0`, filtrage par HiSyncId (voir référence protocole) pour
  reconnaître les deux aides de la paire ciblée.
- Connexion + bonding (stockage des clés LTK en NVS pour survivre aux redémarrages — comme le fait
  Android, sans quoi il faudrait ré-appairer à chaque boot).
- Gestion de la reconnexion automatique en cas de perte de lien (l'aide sort de portée, coupure
  d'alimentation, etc.).
- Doit maintenir **deux connexions BLE indépendantes simultanées** (une par oreillette).

### 3.2 GATT Client / Session ASHA (une instance par aide)

- Découverte du service `0xFDF0` et de ses characteristics par UUID (jamais par handle numérique
  fixe — voir référence protocole §3).
- Lecture de ReadOnlyProperties (validation codec/PSM/HiSyncId au runtime, détection si jamais un
  firmware d'aide différent était rencontré plus tard).
- Lecture de LE_PSM_OUT pour connaître le PSM à utiliser.
- Activation des notifications AudioStatusPoint.
- Écriture Volume, puis séquence START/STOP sur AudioControlPoint (bytes exacts en référence
  protocole §6).
- Remonte un état (`IDLE / STARTING / STREAMING / STOPPING / ERROR`) à l'orchestrateur.

### 3.3 L2CAP CoC Manager (un canal par aide)

- Ouverture du canal sur le PSM annoncé, une fois le service ASHA validé et avant/pendant le START.
- Suivi du **crédit-based flow control** : ne jamais dépasser le crédit disponible côté aide ;
  traiter les trames `LE Flow Control Credit` de réapprovisionnement.
- File d'attente courte de trames sortantes (quelques trames de profondeur max — pas de gros
  buffering, sinon la latence explose).

### 3.4 Pipeline audio (capture + traitement)

- Capture I2S continue depuis l'ADC externe.
- Downmix stéréo→mono (simple moyenne, sauf si l'utilisateur veut préserver un signal spécifique
  d'un canal).
- Rééchantillonnage vers 16 kHz (filtre + décimation/interpolation simple ; pas besoin de qualité
  audiophile, la cible est du G.722 à 64 kbps).
- Détection de signal / silence (voir §3.7).

### 3.5 Encodeur G.722

- Intégrer une implémentation G.722 existante et éprouvée (portage léger, licence permissive —
  évaluer en Phase 5 plutôt que de réécrire un codec from scratch).
- Découpage en trames de 20 ms (160 octets encodés par trame par oreillette, comme mesuré).

### 3.6 Framer ASHA + Séquenceur

- Construit la trame `[0xa1][0x00][seq][160 octets G.722]` (voir référence protocole §7).
- Compteur de séquence **par connexion**, remis à 3 à chaque nouveau START.
- Timing : un tick 20 ms pilote l'envoi vers les deux canaux CoC de façon aussi synchrone que
  possible (les deux aides doivent recevoir leur trame au même rythme pour rester en phase).

### 3.7 Orchestrateur / State Machine

- États globaux : `DISCONNECTED → CONNECTING → READY → STREAMING → STOPPING`.
- Démarrage du stream déclenché par détection de signal audio TV (seuil sur le niveau capté) plutôt
  que par un bouton — évite de laisser le stream ouvert (et donc les aides "occupées") quand la TV
  est éteinte ou muette.
- Arrêt automatique après un délai de silence (ex. quelques secondes) pour libérer les aides pour
  un usage normal (téléphone, environnement) quand la TV ne diffuse rien.
- Gestion des erreurs : reconnexion BLE, ré-ouverture du canal CoC, ré-émission du START si une
  notification d'erreur arrive sur AudioStatusPoint (valeur ≠ `0x00`).

## 4. Points d'incertitude à lever tôt (risques techniques)

1. **API L2CAP CoC côté client dans NimBLE/ESP-IDF** — rôle initiateur (pas juste serveur), sur la
   version d'IDF ciblée. Bloquant pour tout le reste si absent → à valider en tout premier (Phase 0).
2. **Deux connexions BLE + deux canaux CoC simultanés** — vérifier les limites de configuration
   NimBLE (nombre de connexions concurrentes, buffers L2CAP) tiennent la charge sans drop de trame.
3. **Tenue de la cadence 20 ms** sous charge (radio + audio + I2S) — jitter excessif dégraderait le
   rendu audible.
4. **Comportement du volume** — l'appli Signia permet probablement un contrôle utilisateur ; le
   host doit décider s'il expose un réglage de volume (potentiomètre physique ?) ou fixe une valeur
   raisonnable par défaut.

## 5. Hors périmètre (pour l'instant)

- Auracast / LE Audio : non pertinent, ces aides ne l'implémentent pas (confirmé indirectement —
  elles exposent ASHA classique, pas de service LE Audio dans le sniff).
- MFi / iPhone : non pertinent pour ce host (rôle Android/ASHA uniquement).
- Contrôle applicatif Signia (services propriétaires additionnels vus lors du scan GATT) : ignorés,
  non nécessaires pour du streaming audio pur.
