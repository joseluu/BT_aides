# Plan d'implémentation — Relais audio TV vers aides auditives

Chaque phase a un critère de validation clair avant de passer à la suivante — ce projet touche une
couche protocolaire non documentée officiellement côté implémentation host, mieux vaut valider
petit à petit que découvrir un blocage architectural tard.

Référence protocole : [`protocole_asha_reference.md`](./protocole_asha_reference.md).
Architecture détaillée : [`architecture.md`](./architecture.md).

## Phase 0 — Socle projet et levée du risque principal

**Statut : COMPLETE (2026-09-12). Risque API levé par inspection ET par test reussi contre une
vraie aide auditive — canal L2CAP CoC ouvert avec succes sur matériel réel.**

### Preuve materielle finale

Test direct de l'ESP32-C3 contre l'aide auditive gauche reelle (protocole détaillé plus bas) :
1. Detection de l'advertising ASHA (octets bruts `f0 fd 01 02 30 45 43 45` — service 0xFDF0,
   capability=Left/binaural, HiSyncId tronque `30:45:43:45`, conforme a la reference protocole)
2. Connexion GAP etablie
3. **Appairage/bonding reussi** (Just Works, `Encryption change event; status=0`)
4. Decouverte de services GATT effectuee (handles 1-167)
5. **Canal L2CAP CoC ouvert avec succes** :
   `LE COC connected, psm: 0x80, scid: 0x0040, dcid: 0x0040, our_mtu: 512, peer_mps: 247, peer_mtu: 248`
   — `peer_mps`/`peer_mtu` identiques aux valeurs mesurees dans le sniff HCI Android d'origine.

Ca ferme definitivement le risque #1 identifie en architecture.md : NimBLE en role
client/initiateur peut etablir un canal audio ASHA complet (connexion + bonding + CoC) contre une
vraie aide Signia, pas seulement en theorie/sur la spec.

**Piege rencontre et corrige** : la tache de demo d'origine (`ble_coc_cent_task`, herite de
l'exemple `coc_blecent`) envoie des paquets de test de 512 octets sans tenir compte du
`peer_mtu` neqocie — contre notre aide reelle (`peer_mtu=248`), ca echoue
(`Data sending failed, rc=10`) et semble provoquer la fermeture du canal/lien cote aide (l'aide
etait de nouveau visible en advertising quelques dizaines de secondes plus tard). Tache desactivee
dans `main.c` (commentee, pas supprimee — reference gardee). A partir de la Phase 2, le vrai envoi
de trames respectera le MTU reel decouvert dynamiquement, pas une taille fixe copiee de l'exemple.

**Debogage utile a retenir pour la suite** : l'ESP32 continue de scanner/tourner en arriere-plan
entre deux lectures du port serie — une capture ponctuelle avec reset peut couper un evenement en
cours de log. Pour observer une sequence complete, preferer une **lecture continue en flux** (script
qui ouvre le port une fois et ecrit ligne par ligne dans un fichier au fil de l'eau, sans limite de
duree courte) plutot que des fenetres fixes de type "capture 30s puis ferme" — ca decouple
completement l'observation du timing du geste physique sur l'aide, qui reste necessaire (retrait
quelques secondes puis repose/remise ; la simple coupure du Bluetooth du telephone seul n'a pas
suffi dans nos essais). Combiner avec un `tail -f | grep` en arriere-plan sur les marqueurs cles pour
etre notifie sans avoir a relire tout le log a chaque fois.

**Confirmation independante (2eme essai, 2026-09-12)** : nouveau cycle complet reussi sans la tache
de demo (desactivee) — connexion, bonding, et **canal L2CAP CoC ouvert** avec les memes parametres
exacts que le premier essai (`psm=0x80, peer_mps=247, peer_mtu=248`). Le crash intermittent observe
lors du 2e/3e essai (avant la desactivation de la tache de demo) ne s'est pas reproduit.

**3eme confirmation, avec l'aide DROITE cette fois** (capability byte `03` au lieu de `02`) :
meme succes complet (bonding + CoC, memes psm/mtu/mps). Prouve que le mecanisme fonctionne avec les
deux unites physiques independamment, pas seulement avec un seul exemplaire. Phase 0 validee trois
fois sur materiel reel.

**Limite connue du firmware de test actuel** : une seule connexion geree a la fois
(`conn_handle_coc`/`coc_chan` sont des variables globales uniques, pas des tableaux par aide). Si
les deux aides advertisent en meme temps, seule la premiere detectee est engagee — le scan est
annule des qu'une correspondance est trouvee et ne reprend qu'a la deconnexion. Geter les deux
connexions simultanement (une par oreillette) est explicitement du perimetre de la Phase 6, pas de
la Phase 0.

**Objectif** : confirmer que NimBLE/ESP-IDF peut jouer le rôle **client/initiateur** L2CAP CoC (pas
seulement serveur), condition bloquante pour tout le reste.

### Résultat de la levée de risque (inspection directe des sources, sans matériel)

Confirmé par lecture du framework ESP-IDF embarqué dans PlatformIO
(`~/.platformio/packages/framework-espidf`) :
- `components/bt/host/nimble/nimble/nimble/host/include/host/ble_l2cap.h` expose bien
  `int ble_l2cap_connect(uint16_t conn_handle, uint16_t psm, uint16_t mtu, struct os_mbuf *sdu_rx, ble_l2cap_event_fn *cb, void *cb_arg)`
  — le rôle **client/initiateur** existe nativement, pas seulement `ble_l2cap_create_server()`.
- Kconfig : `CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM` (0-9, défaut 0 — doit être explicitement mis à 2
  pour nos deux canaux audio).
- Espressif fournit un **exemple officiel prêt à l'emploi** pour exactement ce rôle :
  `examples/bluetooth/nimble/ble_l2cap_coc/coc_blecent` (licence Unlicense/CC0). C'est la base
  utilisée pour le projet dans `firmware/` plutôt que d'écrire ce squelette from scratch.

### Socle projet posé

- `firmware/` = copie adaptée de `coc_blecent` (composant `nimble_central_utils` inclus) +
  `platformio.ini` ciblant `seeed_xiao_esp32s3` + `sdkconfig.defaults` (NimBLE central,
  `L2CAP_COC_MAX_NUM=2`, flash 8MB).
- **Build validé** (2026-09-12) : compile sans erreur (RAM 8.2%, Flash 6.3% — grande marge).

### Notes d'environnement (à refaire à l'identique la prochaine fois)

- Le paquet global `~/.platformio/packages/tool-esptoolpy` était corrompu (fichiers sources
  manquants, seuls `package.json`/`tools.json` présents) — réparé via
  `pio pkg install -g -t "platformio/tool-esptoolpy" -f`. Si l'erreur
  `ModuleNotFoundError: No module named 'esptool'` réapparaît, c'est le même symptôme.
- **Le build complet ESP-IDF de ce projet ne passe pas de façon fiable sous Git Bash**, même avec
  le shim `~/bin/pio` : `idf_tools.py` (appelé en interne pendant l'étape bootloader/image) réinvoque
  son propre `bash.exe`, qui redéfinit `MSYSTEM` indépendamment de l'environnement scruté par le
  shim, et l'erreur `MSys/Mingw is not supported` revient. **Utiliser le fallback PowerShell décrit
  dans la skill `platformio-espidf`** pour tout `pio run` sur ce projet — c'est la méthode qui a
  fonctionné.
- Après toute modif de `sdkconfig.defaults`, supprimer `firmware/sdkconfig.xiao_esp32s3` avant de
  rebuilder : PlatformIO ne réapplique pas les defaults à un sdkconfig déjà généré.

**Critère de sortie complet** : firmware flashé, boot confirmé par moniteur série. **Fait le
2026-09-12**, mais sur un **XIAO ESP32-C3** et non un S3 : la carte physiquement branchée s'est
révélée être un ESP32-C3 (`esptool chip-id` → "ESP32-C3 (QFN32) revision v0.4", mono-cœur, 4MB
flash), pas un S3 comme supposé au départ. Décision utilisateur : continuer sur ce C3 pour l'instant
et migrer vers un vrai S3 avant les phases audio temps réel si la cadence 20 ms s'avère instable
(risque déjà identifié en architecture.md).

Log de boot confirmé sur COM8 :
```
I (296) main_task: Calling app_main()
I (306) BLE_INIT: BT controller compile version [...]
I (316) BLE_INIT: Bluetooth MAC: 94:a9:90:98:e6:5a
I (376) NimBLE_BLE_CENT_L2CAP_COC: BLE Host Task Started
I (386) NimBLE: GAP procedure initiated: discovery;
```
Le firmware scanne activement en BLE (passive scan, `duration=forever`) — toute la chaîne
build/flash/boot/NimBLE/scan est validée sur matériel réel.

Projet `firmware/` a maintenant deux environnements PlatformIO : `xiao_esp32c3` (actif,
matériel disponible) et `xiao_esp32s3` (conservé pour la migration future). **`sdkconfig.defaults`
est partagé entre les deux** — sa ligne de taille flash (actuellement 4MB) correspond au C3 ; la
remettre à 8MB en repassant sur un vrai S3 (voir commentaire dans le fichier).

**Reste à faire pour clore complètement la Phase 0** : test bout-en-bout du canal L2CAP CoC en tant
que client contre un vrai pair serveur. Deux options envisagées, à trancher avec l'utilisateur :
un second ESP32 flashé avec `coc_bleprph` (exemple officiel jumeau, déjà repéré dans le framework),
ou un test direct contre une des aides réelles (qui coupera temporairement sa connexion au
téléphone).

**Si bloquant** (matériel) : évaluer une bascule vers Bluedroid, ou vers un autre SoC. Aucun
blocage API trouvé à ce stade.

## Phase 1 — Découverte GATT et lecture ASHA (sans streaming)

**Statut : implémenté en code (2026-09-12), pas encore validé sur matériel réel — l'utilisateur
s'est absenté avant de pouvoir refaire le geste sur l'aide.**

**Objectif** : le firmware retrouve et valide les deux aides réelles, sans encore toucher à l'audio.

- Scan filtré sur le service `0xFDF0`, extraction du HiSyncId tronqué depuis l'advertising. **Fait
  (Phase 0).**
- Connexion + bonding sur les deux aides (adresses ou correlation via HiSyncId). **Fait pour une
  aide a la fois (Phase 0) ; les deux simultanement = Phase 6.**
- Découverte des characteristics par UUID (§3 de la référence protocole). **Implémenté** dans
  `blecent_l2cap_coc_on_disc_complete()` (`main.c`) via `peer_chr_find_uuid()`/`peer_dsc_find_uuid()`
  pour les 5 characteristics ASHA (ReadOnlyProperties, AudioControlPoint, AudioStatusPoint + son
  CCCD, Volume, LE_PSM_OUT) — plus de handle en dur.
- Lecture de ReadOnlyProperties et LE_PSM_OUT sur les deux connexions. **Implémenté** (lecture
  dynamique via `ble_gattc_read()`, décodage HiSyncId/codecs/PSM loggé) — **pour une aide à la fois**
  pour l'instant, cohérent avec la limite actuelle du firmware.
- Stockage des clés de bonding en NVS ; test d'un reboot ESP32 qui reconnecte sans ré-appairage.
  **Pas encore testé explicitement** (le stockage NVS des bonds est un comportement par défaut de
  `ble_store_config_init()`, déjà en place depuis Phase 0, mais le test de reconnexion après reboot
  reste à faire).

**Critère de sortie** : logs confirmant, pour les deux aides, HiSyncId = `95:02:44:43:30:45:43:45`,
codec = G.722, PSM = `0x0080`, et reconnexion automatique après reboot sans ré-appairage manuel.
**Reste à valider sur matériel** — le décodage ReadOnlyProperties devrait logger ces valeurs
automatiquement dès la prochaine connexion réussie.

## Phase 2 — Plan de contrôle (START/STOP) sans audio réel

**Statut : implémenté en code (2026-09-12), flashé sur la carte, pas encore validé sur matériel
réel.**

**Objectif** : reproduire exactement la séquence de contrôle observée dans le sniff, avec un flux
audio "factice" (silence encodé, ou tonalité de test) pour valider la mécanique de bout en bout
avant d'investir dans le vrai pipeline audio.

- Activer les notifications AudioStatusPoint. **Implémenté** (écriture de la CCCD après lecture du
  PSM, `asha_on_le_psm_out_read()`).
- Écrire Volume, puis la commande START (`01 01 00 <vol> 01`) sur AudioControlPoint. **Implémenté**
  (`asha_on_cccd_write()` → `asha_on_volume_write()` → écriture START).
- Vérifier réception d'un ack `00` sur AudioStatusPoint. **Implémenté** — nouveau cas
  `BLE_GAP_EVENT_NOTIFY_RX` dans `blecent_gap_event()`, distingue ack de START vs ack de STOP via un
  état `enum asha_state` (`IDLE / AWAITING_START_ACK / STREAMING / AWAITING_STOP_ACK`).
- Ouvrir le canal L2CAP CoC sur le PSM lu en Phase 1, avec suivi des crédits. **Implémenté**, avec
  correction d'ordonnancement — voir "Bugs trouvés et corriges en direct" ci-dessous.
- Envoyer des trames factices au format exact (`0xa1 0x00 <seq> <160 octets>`) toutes les 20 ms,
  séquence redémarrant à 3. **Implémenté** dans `asha_audio_test_task()` — envoie 150 trames (~3s),
  payload à `0x00` fixe (PAS du vrai G.722, juste pour valider le transport/cadence/crédits ; le vrai
  encodeur arrive en Phase 4/5).
- Écrire STOP (`02`), vérifier l'ack. **Implémenté** (`asha_send_stop()`, appelée automatiquement à
  la fin des 150 trames de test).

### Session de debug en direct (2026-09-12, soir) — 3 bugs trouves et corriges

Grace a la methode de capture en flux + `tail -f | grep` (voir Phase 0), plusieurs essais reels
consecutifs ont ete faits dans la meme session, chacun reflashe immediatement apres correction :

1. **`AudioStatusPoint introuvable`** — le pool interne du module `peer` (`peer_init(..., 64, 64,
   64)`) etait trop petit pour la table GATT complete de l'aide (services proprietaires Signia
   nombreux avant d'atteindre 0xFDF0). Corrige en augmentant les tailles
   (`peer_init(MYNEWT_VAL(BLE_MAX_CONNECTIONS), 20, 120, 60)`).
2. **`ecriture Volume echouee, status=508`** — la characteristic Volume est "Write No Response"
   uniquement (confirme dans le sniff Android : "Write Command"), mais le code utilisait
   `ble_gattc_write_flat()` (Write Request, qui attend une reponse) au lieu de
   `ble_gattc_write_no_rsp_flat()`. Corrige — l'ecriture Volume n'a plus de callback, on enchaine
   directement sur l'ecriture START dans la meme fonction (`asha_on_cccd_write()`).
3. **`LE COC error: 6` (Insufficient Authorization)** — bug d'ordonnancement plus subtil. Le code
   ouvrait le canal L2CAP CoC APRES l'ack du START, alors que le sniff Android montre clairement
   l'ordre inverse (CoC ouvert AVANT CCCD/Volume/START — voir protocole_asha_reference.md §6).
   Corrige (le canal s'ouvre juste apres la lecture du PSM, `asha_start_control_sequence()` prend le
   relais une fois `BLE_L2CAP_EVENT_COC_CONNECTED` recu). **Ce correctif seul n'a pas suffi** :
   l'erreur 6 est revenue meme avec le bon ordre. Hypothese retenue apres analyse des timings :
   `peer_disc_all()` (decouverte complete de tous les ~9 services, y compris les proprietaires
   Signia) prend 30-40 secondes avec le pool agrandi du point 1, contre ~8 secondes lors du succes
   du test Phase 0 (ou la decouverte etait tronquee par manque de memoire). L'aide semble n'autoriser
   l'ouverture du canal audio que dans une fenetre de temps limitee apres la connexion/bonding — un
   vrai client Android ne re-decouvre certainement pas les services proprietaires du fabricant a
   chaque reconnexion. **Corrige** en remplacant `peer_disc_all()` par
   `peer_disc_svc_by_uuid(conn_handle, &asha_svc_uuid.u, disc_cb, NULL)` — decouverte ciblee sur le
   seul service ASHA, qui devrait etre bien plus rapide.

**Suite (meme soiree, apres retour de l'utilisateur)** : la decouverte ciblee a ete testee — elle
fonctionne comme prevu (discovery en ~1.4s au lieu de 30-40s), mais **`LE COC error: 6` persiste a
l'identique**. Un nouveau probleme est aussi apparu entre-temps : apres plusieurs heures de
fonctionnement continu de l'ESP32 sans reboot, un essai a echoue avec
`Encryption change event; status=1288` (= `BLE_HS_ERR_SM_PEER_BASE + 8` = echec SM cote peer,
raison 8 "Unspecified Reason") — corrige en effacant la partition NVS de l'ESP32
(`python -m esptool --port COM8 erase_region 0x9000 0x6000`), qui contenait un bond obsolete.
Apres cet effacement, le bonding a de nouveau reussi proprement (status=0).

**`LE COC error: 6` reste inexplique apres 3 hypotheses testees et corrigees sans effet** :
1. Ordre CoC/START (corrige — canal ouvert avant START, conforme au sniff) : erreur persiste.
2. Vitesse de decouverte (corrige — decouverte ciblee ~1.4s, plus rapide que le succes original de
   Phase 0 a ~8s) : erreur persiste, identique.
3. Bonding obsolete (corrige — NVS effacee, bonding frais confirme status=0) : erreur persiste,
   identique.

Le fait que l'erreur reste **exactement la meme (code 6)** malgre des changements structurels
significatifs cote firmware suggere que la cause n'est **peut-etre plus cote code** : apres une
dizaine de tentatives de connexion/appairage contre la meme aide en une seule soiree, hypothese
retenue = **verrou de securite/anti-abus cote aide** (comportement defensif courant sur ce type
d'appareil apres des tentatives repetees en peu de temps), pas un bug dans notre sequence. A tester
au prochain essai : laisser reposer l'aide plus longtemps (ex. cycle de charge complet, ou plusieurs
dizaines de minutes sans sollicitation) avant de retenter, pour voir si l'erreur disparait d'elle-meme
sans changement de code. Si elle persiste meme apres repos, il faudra reconsiderer une cause
protocolaire plus profonde (a ce stade, un nouveau sniff HCI Android frais sur cette meme aide
serait le moyen le plus fiable de trancher).

**Statut a la fin de la session** : Phase 1 et le control-plane START (jusqu'a l'ack) sont
entierement valides en direct, de facon repetee. Seule l'ouverture du canal CoC en aval du START
bloque, avec une cause qui reste incertaine malgre plusieurs corrections legitimes deja appliquees.

**Critère de sortie** : les aides émettent un son audible (même du silence "propre" ou un bip de
test) pendant la fenêtre START/STOP, sans erreur de statut ni coupure de connexion. **Toujours pas
validé sur matériel** au sens complet (canal CoC + trames + STOP) à cause du bug d'ordonnancement/
timing ci-dessus, mais tout le reste de la chaîne (Phase 1 entière + control-plane START jusqu'à
l'ack) l'a été avec succès à plusieurs reprises en direct.

### Round 2 -- sniff HCI Android frais en session MONO-AIDE (2026-09-12, tard le soir)

L'utilisateur a fait une observation cle : il a deja vecu du streaming Android vers **une seule**
aide (cas d'une aide a plat), donc l'hypothese "il faut forcement les deux aides" (envisagee un
temps) est fausse. Il a propose de refaire un sniff HCI Android dans ce scenario exact -- bonne
decision, ca a debloque plusieurs points :

1. **`otherstate` confirme = `0x00` en mono-aide** (commande START vue : `01 01 00 ec 00`) -- le
   correctif deja applique etait le bon.
2. **Deux ecritures "mystere" sur AudioControlPoint reperees** : `03 02 10` avant l'ouverture CoC,
   et `03 00` juste avant START. Testees **separement** pour isoler les variables :
   - `03 02 10` (avant CoC) : **a fait ressurgir `LE COC error: 6`** qui avait disparu sans elle --
     retiree, ne pas la reintroduire sans comprendre pourquoi elle nuit.
   - `03 00` (avant START) : neutre, aucun effet observe dans un sens ou l'autre -- conservee sans
     certitude sur son utilite reelle.
3. **Format des trames audio reconfirme** identique (`a1 00 <seq> ...`, seq demarre a 3, premiere
   trame arrive 1 seul paquet apres l'ack de START chez Android).
4. **Decouverte majeure** : Android **renegocie l'intervalle de connexion BLE** juste apres la
   connexion GAP (avant meme le bonding) via `HCI_LE_Connection_Update` -- passe de l'intervalle par
   defaut a **20ms/20ms, latency=10, timeout=3000ms**. Notre firmware ne le faisait jamais (restait
   sur l'intervalle par defaut ~30-50ms, latency=0). Hypothese retenue : l'aide attend un lien deja
   adapte au streaming temps reel avant d'accepter de streamer, ce qui expliquerait le silence
   total sur l'ack de START malgre une commande bien recue (write ATT reussi) a chaque essai.
   **Corrige** : ajout d'un appel `ble_gap_update_params()` dans `BLE_GAP_EVENT_CONNECT`, avec les
   memes valeurs exactes qu'Android (itvl_min=itvl_max=16 soit 16*1.25ms=20ms, latency=10,
   supervision_timeout=300 soit 300*10ms=3000ms).

**Bug annexe repere (non bloquant, a corriger plus tard)** : le handler `BLE_GAP_EVENT_NOTIFY_RX`
compare seulement `attr_handle` sans verifier `conn_handle` -- une notification d'un tout autre
appareil BLE partageant le meme numero de handle serait mal attribuee a l'aide. Explique les
notifications "spontanees" `status=0x00 (etat=0)` observees meme sans connexion active. A corriger
dans `main.c` en ajoutant la verification du conn_handle avant Phase 3.

### Round 3 -- la renegociation d'intervalle a debloque l'ack, nouveau symptome isole (2026-09-13)

**La renegociation d'intervalle (Round 2) a fonctionne** : testee en direct, l'ack de START arrive
enfin (`notification AudioStatusPoint, status=0x00`) juste apres l'ecriture de START -- exactement
comme dans le sniff Android. Progres reel : on depasse enfin le point de blocage precedent.

**Nouveau symptome, isole precisement par elimination** : des que le firmware tente d'envoyer sa
premiere trame audio sur le canal CoC (`ble_l2cap_send()`), l'aide **coupe la connexion
~30ms plus tard** (`LE CoC disconnected` puis `disconnect; reason=531`), sans jamais rapporter
d'erreur locale (`ble_l2cap_send()` retourne toujours 0 = succes cote ESP32). Trois tests, memes
resultats :
1. Payload de test = zeros -> deconnexion a ~30ms apres l'envoi
2. Payload = **vraie trame G.722 capturee dans le sniff Android** (octets exacts du paquet [pkt
   2691], premiere trame reelle apres un ack de START) -> **meme deconnexion a ~30ms**, ecarte donc
   l'hypothese "contenu invalide"
3. Ajout d'un delai artificiel de 100ms avant le tout premier envoi -> la deconnexion arrive
   simplement ~100ms plus tard (~130ms apres l'ack au lieu de ~30ms) -> ecarte l'hypothese d'une
   condition de course liee au timing ; confirme que c'est bien l'ACTE D'ENVOYER qui declenche le
   rejet, environ 30ms apres, systematiquement.

**Conclusion a ce stade** : le probleme n'est plus dans la sequence de controle GATT (entierement
validee, ack recu) ni dans le contenu du payload testee. Il reste soit dans la structure bas niveau
de la trame L2CAP telle qu'emise par notre ESP32 (segmentation SDU, en-tete, credits — invisible
dans nos logs, qui ne montrent que ce que *notre* code croit avoir envoye, pas les octets reels sur
l'air), soit un comportement de l'aide non encore compris.

**Outil manquant pour la suite** : un vrai **sniffer BLE captant le trafic radio de l'ESP32
lui-meme** (pas celui d'Android, qu'on a deja) est necessaire pour voir les octets reellement emis
sur l'air et les comparer avec la reference Android au niveau bit. Le snoop HCI Android ne capture
que le trafic OU Android est lui-meme un des deux points — un nRF52840 en mode sniffer (Wireshark +
plugin "nRF Sniffer for Bluetooth LE") ou equivalent serait la methode la plus fiable.

**Bug annexe repere (non bloquant, a corriger plus tard)** : le handler `BLE_GAP_EVENT_NOTIFY_RX`
compare seulement `attr_handle` sans verifier `conn_handle` -- une notification d'un tout autre
appareil BLE partageant le meme numero de handle serait mal attribuee a l'aide. Explique les
notifications "spontanees" `status=0x00 (etat=0)` observees meme sans connexion active. A corriger
dans `main.c` en ajoutant la verification du conn_handle avant Phase 3.

### Round 4 -- test de reference avec un serveur controle (2026-09-13)

Au lieu d'un sniffer BLE (non disponible), test alternatif : deux ESP32-S3 supplementaires
identifies (COM12 et COM13). Un serveur L2CAP CoC generique (`coc_bleprph`, exemple officiel non
modifie) flashe sur COM13 (`firmware_test_server/`), et un client (`coc_blecent` officiel, seul le
contenu de la trame envoyee modifie pour reproduire EXACTEMENT la trame qui echoue contre l'aide —
163 octets, en-tete `a1 00 03` + le meme vrai payload G.722 capture dans le sniff Android) flashe
sur COM12 (`firmware_test_client/`).

**Resultat : succes complet.** `LE COC connected` puis 5x `Data sent successfully`, connexion stable
sur toute la duree du test, **aucune deconnexion**. La toute meme trame qui provoque un rejet en
~30ms contre l'aide reelle passe sans probleme contre un serveur L2CAP CoC generique.

**Conclusion definitive** : le mecanisme d'envoi bas niveau de notre client
(`os_mbuf_get_pkthdr`/`os_mbuf_append`/`ble_l2cap_send`) est **correct**, prouve independamment de
l'aide. Le rejet observe est un **comportement specifique a l'aide Signia**, pas un bug de notre
implementation. Reste a determiner : quelle validation/attente supplementaire l'aide applique avant
d'accepter une trame audio, que notre sequence actuelle (bonding + decouverte ciblee + intervalle
20ms + CCCD + Volume + `03 00` + START + ack) ne satisfait pas encore.

**Pistes non encore explorees pour la suite** :
- Reexaminer le sniff Android **binaural** d'origine (premiere capture, deux aides) pour tout detail
  manque entre l'ack de START et la toute premiere trame reelle — notamment si un evenement L2CAP ou
  ATT supplementaire s'intercale que le parseur actuel ne decode pas encore (ex. reconfiguration MPS/
  MTU post-connexion, signaling L2CAP additionnel).
- Verifier si l'aide attend une paire coordonnee (meme si le streaming mono-aide est possible cote
  utilisateur/Android, peut-etre que le declenchement du flux reel differe subtilement en mono).
- Envisager de contacter/chercher une documentation communautaire sur le firmware Signia specifique,
  ou tester avec une autre paire d'aides ASHA d'un autre fabricant si disponible, pour voir si le
  comportement est generique ASHA ou specifique Signia.

**Projets `firmware_test_server/` et `firmware_test_client/`** : conserves dans le repertoire du
projet comme outils de diagnostic reutilisables (COM13 doit etre rendu pour un autre usage — la
carte sur **COM12 est destinee a devenir la carte definitive du projet ASHA**, remplacant le C3, une
fois ce diagnostic termine).

### Round 5 -- meme firmware teste sur un vrai ESP32-S3 (2026-09-13, tard)

Pour eliminer l'hypothese d'un probleme de timing/ordonnancement specifique au C3 mono-coeur,
le firmware ASHA actuel (identique, avec le delai de test de 100ms toujours present) a ete
recompile et flashe sur un vrai ESP32-S3 generique (devkit nu, trouve dans la soiree, COM12 —
nouvel environnement `esp32s3_com12` dans `firmware/platformio.ini`).

**Resultat : echec identique.** Meme sequence complete jusqu'a l'ack de START, meme demarrage du
test audio, puis **meme deconnexion quelques dizaines de ms plus tard** (`reason=531`), au meme
ecart net par rapport au delai artificiel qu'observe sur le C3. Reproductible a l'identique sur
un chip totalement different (dual-core vs mono-core).

**Conclusion definitive** : le comportement n'a rien a voir avec le materiel ESP32 (C3 ou S3) ni
avec un probleme d'ordonnancement FreeRTOS. C'est un comportement de l'aide, point final. Les
seules pistes restantes sont protocolaires (Round 4) : reexaminer le sniff Android existant pour
un detail manque, ou obtenir un sniffer BLE dedie pour voir le trafic reel emis par l'ESP32 et le
comparer octet pour octet avec Android au niveau radio (nRF52832 nu identifie chez l'utilisateur —
prevoir prochaine session : tracage des broches SWD/UART au multimetre a partir du pinout officiel
Nordic QFN48 — SWDCLK=broche 25, SWDIO=broche 26, nRESET=broche 24, VDD=broches 13/36, VSS=broche
31 — avant flashage du firmware sniffer officiel).

### Round 6 -- lecture du code source d'un host ASHA tiers (2026-09-13, tres tard)

Suggestion de l'utilisateur : les sources d'Android (AOSP) pourraient-elles aider ? Recherche menee
sur un projet tiers independant, `asha_pipewire_sink` (github.com/thewierdnut/asha_pipewire_sink),
qui implemente un host ASHA complet pour Linux/PipeWire — le meme probleme que nous, deja resolu et
documente par quelqu'un d'autre. Fichier cle : `asha/Side.cxx` (machine a etats par oreillette).

**Trouvaille 1 -- la commande mystere `03 02 10` expliquee** : c'est un message **STATUS**
(`Control::STATUS=3`) signifiant "parametres de connexion mis a jour" :
```cpp
m_char.audio_control.Command({Control::STATUS, Update::PARAMETERS_UPDATED, interval});
// = {0x03, 0x02, interval} -- interval=16 (16*1.25ms=20ms) => "03 02 10" exactement
```
A envoyer **apres** la renegociation d'intervalle de connexion (deja faite chez nous), pour informer
l'aide du nouvel intervalle. Notre tentative precedente (Round 4) l'envoyait AVANT l'ouverture du
canal CoC, ce qui cassait le CoC (`LE COC error: 6`) — a ressayer **apres** l'ouverture du canal,
comme dans la reference.

**Trouvaille 2 -- explication tres probable du blocage principal**, tiree d'un commentaire du
developpeur dans le code (`OnHAPropChanged`, characteristic proprietaire `38278651-76d7-4dee-83d8-
894f3fa6bb99`, absente de la spec ASHA officielle) :
```
Only right HA turned on: ACP start goes to 3 (syncing) at the same time as asha status ok,
then *no audio*. No audio for several seconds.
Turn on left HA *but don't connect left to bluetooth*, goes to 1, audio starts.
```
**Une aide binaurale attend de detecter sa jumelle active (via un lien propriataire oreille-a-
oreille, ex. NFMI, independant du Bluetooth) avant d'accepter de streamer reellement** — meme apres
avoir accepte START et ouvert le canal CoC. La jumelle n'a PAS besoin d'etre connectee en Bluetooth,
juste portee et allumee. Ceci correspond probablement au souvenir de l'utilisateur (streaming reussi
avec une aide a batterie faible : l'autre etait sans doute encore portee/allumee, donc detectee via
NFMI, meme si Android ne s'y etait pas connecte en Bluetooth).

**Test propose pour la prochaine session** : porter les DEUX aides (allumees, aux oreilles), mais ne
laisser l'ESP32 se connecter en Bluetooth qu'a UNE seule. Si l'hypothese est juste, le streaming
devrait enfin fonctionner sur celle qui est connectee.

**Autres pistes utiles trouvees dans `Side.cxx`** (a integrer si le test ci-dessus confirme
l'hypothese) :
- Sequence de connexion complete : ouverture socket L2CAP CoC (`SOCK_SEQPACKET` + `BT_MODE_LE_FLOWCTL`)
  -> renegociation intervalle -> `UpdateConnectionParameters` (STATUS 03 02 xx) -> lecture proprietes
  -> lecture PSM -> connexion CoC.
- Le format START/STOP confirme identique au notre : `{START, codec=1, audiotype=0, volume, otherstate}`.
- `WriteAudioFrame` : trame de 161 octets exactement (`static_assert(sizeof(packet) == 161`) — 1 octet
  de sequence + 160 octets G.722 (PAS de bytes d'en-tete fixes `0xa1 0x00` inclus dans cette struct
  cote emission ; a verifier si ces 2 octets sont ajoutes ailleurs ou si notre decodage du sniff
  differe legerement — point a reverifier avant Phase 4).
- `m_ready_to_receive_audio` n'est mis a `true` qu'apres reception de `STATUS_OK` sur la notification
  — confirme notre approche "attendre l'ack avant de streamer" (pas "envoyer immediatement").

**Test de l'hypothese "jumelle active" (2026-09-13, tres tard)** : teste en direct, protocole
correct confirme par l'utilisateur (deux aides stables aux oreilles, allumees, sans y toucher
pendant 20-30s, geste de declenchement uniquement sur l'aide cible). **Resultat : echec identique**
(meme deconnexion `reason=531` ~30-60ms apres le debut de l'envoi audio), reproduit 2 fois (aide
gauche puis aide droite).

**Cette hypothese precise ne semble donc pas etre la cause de notre probleme** — nuance importante
en relisant le commentaire source : le symptome decrit dans `asha_pipewire_sink` est **"reste bloque
en syncing (etat 3), pas d'audio pendant plusieurs secondes, mais la connexion reste etablie"** —
un symptome de blocage SANS deconnexion. Le notre est une **coupure dure et rapide** (30-60ms), donc
categoriquement different. La synchronisation inter-aides reste une vraie caracteristique du
protocole a garder en tete pour la Phase 6 (les deux connexions Bluetooth simultanees seront de
toute facon necessaires pour l'usage reel), mais elle n'explique probablement pas ce blocage precis.

### Round 7 -- LE BUG TROUVE, via lecture approfondie de asha_pipewire_sink (2026-09-13, tres tard)

Poursuite de la lecture du code source pendant que l'utilisateur cablait le sniffer nRF52832.
Fichier `asha/AudioPacket.hh` :
```cpp
struct AudioPacket {
   static constexpr size_t SIZE_BYTES = 160;
   uint8_t seq;
   uint8_t data[SIZE_BYTES];
} __attribute__((packed));
```
**La vraie trame ASHA au niveau applicatif fait 161 octets exactement : {seq, 160 octets G.722}.
Aucun prefixe `0xa1 0x00`.**

**Le `0xa1 0x00` qu'on croyait etre un en-tete ASHA est en realite le champ de longueur SDU du
L2CAP CoC** : 161 en little-endian = `0xa1 0x00`. C'est ajoute AUTOMATIQUEMENT par la pile L2CAP
lors de l'envoi (premiere K-frame d'une nouvelle SDU), pas quelque chose que l'application doit
construire elle-meme.

**Notre bug depuis le debut de la Phase 2** : `asha_audio_test_task()` construisait a la main un
buffer de 163 octets `[0xa1, 0x00, seq, ...160 octets]` et le passait EN ENTIER a
`ble_l2cap_send()`. NimBLE, ne sachant pas qu'on avait deja "prefixe" nous-memes, rajoutait PAR-DESSUS
son propre prefixe de longueur SDU (163 => `0xa3 0x00`), envoyant reellement 165 octets sur l'air,
avec tout le contenu decale de 2 octets par rapport a ce qu'attend l'aide. Une trame structurellement
incoherente, rejetee immediatement par un parseur ASHA correct — ce qui explique parfaitement :
- Le rejet quasi instantane (~30-40ms) et systematique par l'aide reelle, peu importe le contenu
  (zero ou vrai G.722) ou un delai ajoute avant l'envoi (Round 3).
- Le succes total contre le serveur de test generique (Round 4) : ce serveur ne fait aucune
  validation de structure ASHA, il logue juste les octets bruts recus, peu importe leur coherence.

**Corrige** dans `main.c` : `asha_audio_test_task()` construit maintenant `frame[0]=seq`,
`frame[1..160]=payload G.722`, taille totale 161 octets — laisse `ble_l2cap_send()`/NimBLE gerer
seul le prefixe de longueur SDU. Supprime aussi le delai de diagnostic de 100ms (plus necessaire,
la cause n'etait pas un probleme de timing). Build + flash confirmes sur le S3 (COM12).

**Pas encore teste en direct** — a faire en priorite absolue au prochain essai, avant toute autre
piste (sniffer BLE, etc.). Si confirme, c'est la piece manquante depuis le tout debut de la Phase 2
et le relai audio complet (START -> trames -> STOP) devrait enfin fonctionner de bout en bout.

**Lecon a retenir** : quand on reconstruit un protocole depuis un sniff HCI sans specification
ecrite complete, il faut se mefier des octets qui pourraient appartenir a une COUCHE INFERIEURE
(ici L2CAP) plutot qu'a la couche applicative qu'on croit observer — la lecture d'une implementation
de reference tierce (meme dans un langage/environnement different, ici du C++ Linux/PipeWire pour du
firmware ESP32) a permis de trancher la ou des dizaines d'essais empiriques n'y arrivaient pas.

**VALIDATION EN DIRECT : SUCCES COMPLET** (2026-09-13, meme soiree). Cycle entier observe sans
aucune erreur ni deconnexion :
```
START acquitte -> demarrage du test audio
[150 trames envoyees sur ~3.01s, aucune deconnexion]
fin du test audio -> envoi STOP, rc=0 (plus rc=7/ENOTCONN comme avant !)
notification AudioStatusPoint, status=0x00 (etat=3) -> STOP acquitte
```
**Phase 2 est officiellement complete.** Le bug de la trame corrompue (Round 7) etait bien la cause
unique du blocage qui a resiste a toute la nuit. Toute la chaine ASHA fonctionne desormais de bout
en bout : detection -> connexion -> bonding -> decouverte ciblee -> canal CoC -> notifications ->
Volume -> START -> streaming reel de 150 trames -> STOP -> ack, sur du materiel reel, contre une
vraie aide Signia IX.

**Prochaines etapes (Phase 3+)** : pipeline audio d'entree reel (I2S depuis la TV), encodeur G.722
reel (le contenu actuel est un echantillon statique reutilise en boucle, pas un flux encode en
temps reel), puis Phase 6 (les deux oreillettes simultanement). Le gros risque protocolaire du
projet est leve.

## Phase 3 — Pipeline audio d'entrée

**Objectif** : capter proprement l'audio TV et le préparer pour l'encodage, indépendamment du BLE.

**Câblage I2S fait (2026-10-04)** : breakout ADC PCM1808 connecté à la carte définitive du projet,
un **ESP32-S3 Supermini** (broches sérigraphiées par numéro de GPIO direct, pas un XIAO).

| PCM1808 | ESP32-S3 Supermini | Sens |
|---|---|---|
| MCLK (SCKI) | — | **Non connecté** : le breakout a son propre quartz 24,576 MHz ; ce fil, voisin de BCK dans la nappe, perturbait BCK par diaphonie (GPIO4 libre) |
| BCLK (BCK) | GPIO5 | PCM1808 → ESP32 (6,144 MHz, PCM1808 maître) |
| LRCK (WS) | GPIO6 | PCM1808 → ESP32 (fs = 96 kHz) |
| DATA (DOUT) | GPIO7 | PCM1808 → ESP32 (RX) |
| GND | GND | commun |
| VCC | 5V | Breakout alimenté en 5V (VCC analogique du PCM1808 = 4.5-5.5V) ; régulateur intégré → VDD numérique mesuré à 3.3V, donc sorties logiques sûres pour l'ESP32-S3 |

GPIO choisis pour éviter les pins de strapping (0/3/45/46), l'USB natif (19/20) et l'UART0 (43/44).
Firmware : I2S **esclave**, RX seul, format Philips/standard — le breakout est câblé en mode maître (voir « RÉSOLU » plus bas).
FMT/MD du PCM1808 supposés déjà strappés en I2S standard sur ce breakout (non vérifié explicitement).

### Firmware de test I2S créé et premier essai en direct (2026-10-04)

**Projet séparé** `firmware_i2s_test/` (même convention que `firmware_test_server/` et
`firmware_test_client/` des phases précédentes) — garde `firmware/` (Phase 2, validé) intact.
Driver `driver/i2s_std.h` (ESP-IDF 5.5.2, composant `esp_driver_i2s`), capture en maître I2S,
16 kHz natif (pas de rééchantillonnage logiciel), Philips stéréo 32 bits/slot, extraction de
l'échantillon 24 bits signé par `>> 8` sur chaque mot brut. Affiche un niveau peak/RMS en dBFS
toutes les ~0.5s — couvre le critère de sortie alternatif "mesure au niveau du signal numérique".

**Carte physique** : même ESP32-S3 Supermini que `firmware/` (COM12), mais réapparaît sous un
port COM différent après reflash/replug — ici **COM15** (comportement déjà documenté : le port
change à chaque rebranchement). `esptool flash_id` confirme la puce : ESP32-S3 (QFN56 v0.2),
**flash embarquée 4MB** (pas 8MB comme le board PlatformIO `esp32-s3-devkitc-1` le suppose par
défaut — corrigé via `board_build.flash_size = 4MB` dans `platformio.ini`), **PSRAM embarquée
2MB**, mode natif **USB-Serial/JTAG** (VID/PID `303A:1001`, pas de puce pont UART externe — c'est
la signature caractéristique d'un vrai "Supermini").

**Bug trouvé et corrigé avant le premier essai concluant** : la tâche de capture créée avec une
pile de seulement 4096 octets faisait un **stack overflow** (buffer brut de 2 Ko + profondeur
d'appel de l'init du driver I2S + un `printf`) — corrompait silencieusement le tas, crash
retardé et sans rapport apparent observé dans le idle task watchdog (`LoadProhibited` dans
`find_entry_from_task_handle_and_check_all_reset`, résolu via `addr2line` sur le `.elf`).
**Corrigé** : buffer rendu `static` (sort de la pile) + pile de la tâche portée à 8192 octets.
Plus aucun crash après correctif, capture tourne en continu.

**Premier essai en direct — résultat suspect, pas encore concluant** : le niveau mesuré reste
bloqué en permanence près de la pleine échelle (peak ≈ -0.0 dBFS à chaque bloc, RMS ≈ -8 dBFS,
sur plusieurs secondes consécutives), alors qu'aucune source audio n'était branchée côté PCM1808
à ce stade. Le dump des premiers mots bruts montre des valeurs incohérentes d'un échantillon à
l'autre (octet de poids fort très variable : `0x82`, `0x1a`, `0x9e`, `0x00`...), ce qui ne
ressemble ni à du silence propre (devrait rester proche de 0x00/0xFF) ni à un vrai signal audio
structuré. **Hypothèse principale : DATA (GPIO7) non effectivement raccordé ou PCM1808 non
alimenté** — une entrée flottante sur l'ESP32 capterait du bruit incohérent de ce type.
**Test de diagnostic prioritaire pour la prochaine session** : vérifier au multimètre que le
PCM1808 est bien sous 3.3V, puis court-circuiter temporairement les entrées analogiques L/R du
PCM1808 à la masse — si le niveau mesuré ne descend pas vers un vrai silence (dBFS très négatif),
le problème est côté câblage/alimentation, pas côté code firmware (le driver I2S lui-même tourne
sans erreur ni crash).

### Diagnostic au scope TDS784A (2026-10-05) — cause trouvée : quartz 24,576 MHz sur le breakout

Mesures au scope (CH1=MCLK, CH2=BCK, CH3=LRCK, CH4=DATA, captures analysées point par point) :
- BCK propre, 1,024 MHz. Mais DATA changeait sur une grille fixe de **163 ns** (≈ 6,144 MHz),
  à des instants sans rapport avec BCK — **et cette grille n'a pas bougé quand le MCLK de
  l'ESP32 est passé de 6,144 à 4,096 MHz**. DATA était donc cadencée par une autre horloge.
- Cause : **le breakout a son propre quartz 24,576 MHz** (confirmé visuellement) qui attaque
  SCKI. L'ESP32 pilotait la broche MCLK du connecteur, vraisemblablement contre cet oscillateur
  (niveaux étalés sur CH1), et ses BCK/LRCK n'étaient pas synchrones du SCKI réel → sortie du
  PCM1808 incohérente. Ça explique a posteriori tous les symptômes (niveaux bloqués quel que
  soit le volume, motifs répétitifs, aucun effet des ratios MCLK).
- Pièges de mesure rencontrés : pinces de masse des sondes non reliées (signaux centrés sur 0 V),
  sondes x10 non reconnues sur CH2/CH4 (échelle /10 manuelle). Voir skill `scope`.

**Correction** (`firmware_i2s_test`) : GPIO4 en **entrée**, source d'horloge I2S externe
(`I2S_CLK_SRC_EXTERNAL`, 24 576 000 Hz), **fs = 48 kHz** (512 fs ; 16 kHz impossible avec ce
quartz, le PCM1808 esclave n'acceptant que 256/384/512 fs), BCK = 3,072 MHz. La décimation
48 → 16 kHz (passe-bas + 1 échantillon sur 3) devient une étape logicielle de la Phase 3.
Attention : sans horloge présente sur GPIO4, `i2s_channel_enable()` boucle indéfiniment
(attente de `i2s_ll_rx_update`) et le task watchdog se déclenche — à garder en tête si le
breakout est débranché. **Pas encore testé avec le breakout branché.**

### RÉSOLU (2026-10-05) — PCM1808 en mode maître, ESP32 esclave I2S

La correction « horloge externe 48 kHz » ci-dessus n'était qu'une étape : en la testant, la cadence
mesurée était juste mais chaque mot de 32 bits contenait deux valeurs de 16 bits voisines (lecture
d'un bit sur deux d'un flux deux fois plus rapide). Recoupé avec la grille de 163 ns vue au scope
(24,576 MHz / 4 = 6,144 MHz) : **le breakout est câblé en mode MAÎTRE 256 fs** (MD0/MD1 à 1,
non exposés). C'est le PCM1808 qui génère BCK = 6,144 MHz et LRCK = fs = **96 kHz** ; toutes les
configurations précédentes pilotaient ces lignes contre lui (d'où les « niveaux multiples » sur LRCK).

**Configuration validée** (`firmware_i2s_test`) : `I2S_ROLE_SLAVE`, BCK (GPIO5) et LRCK (GPIO6) en
entrées, DATA (GPIO7), MCLK non utilisé, fs = 96 kHz, Philips stéréo 32 bits/slot, `>> 8`.

**Second problème, matériel** : avec BCK à 6,144 MHz, le fil MCLK de la nappe (24,576 MHz du quartz,
voisin de BCK) corrompait BCK par diaphonie — un essai a perdu la synchro (blocs à 1,3-1,6 s au
lieu de 0,5 s, valeurs figées). **Fil MCLK débranché → stable.**

**Résultats** : silence = RMS -95 dBFS / pic -81 dBFS (bruit de fond normal d'un ADC 24 bits) ;
musique à 10 % sur la prise casque d'un PC = RMS -44 à -47 dBFS, pic ≈ -36 dBFS, sans écrêtage ;
20 s consécutives sans aucun bloc corrompu, cadence exacte (48 128 échantillons / ~505 ms).

**Options du breakout** (sérigraphie au dos, photo dans le dépôt public) : OP2/OP3 ouverts =
maître 96 kHz (usine) ; OP2 court-circuité = maître 48 kHz ; OP2+OP3 = esclave. OP1 ouvert = I2S
24 bits, court-circuité = cadré à gauche. **Choix : maître 48 kHz (pont OP2 soudé), validé** —
BCK 3,072 MHz, silence -92 dBFS, musique à 50 % = -23 à -30 dBFS RMS sans écrêtage, cadence
exacte. Niveau vérifié comme suivant le volume (10 % → 50 % : +12 à +20 dB).

**Dépôt public** de cette base de capture (firmware + README détaillé + photos) :
https://github.com/joseluu/pcm1808_esp32 (copie locale `BT_aides/pcm1808_esp32/`, exclue du dépôt
principal).

**Décimation 48 → 16 kHz : faite et validée (2026-10-05).** FIR equiripple de 80 coefficients
(`scipy.signal.remez`, table `firmware_i2s_test/main/fir_48k_16k.h`) : bande passante 0-7 kHz
(ondulation 0,05 dB), bande coupée ≥ 9 kHz (≥ 70 dB). La bande 8-9 kHz se replie entre 7 et 8 kHz,
hors de la bande utile du G.722, ce qui évite les ~200 coefficients qu'exigerait une coupure à
8 kHz. Exécuté par `dsps_fird_f32` d'ESP-DSP (composant géré `espressif/esp-dsp`, version
optimisée S3 : coefficients multiples de 4, tableaux alignés sur 16 octets ; `len` = nombre
d'échantillons de SORTIE). Auto-test au démarrage sur sinusoïdes synthétiques : 0,0 dB à 1/5/7 kHz,
-1,8 dB à 7,5 kHz, -70,5 dB à 9 kHz, -82 dB à 10 kHz. Sur musique : RMS à 16 kHz 0,1-0,6 dB sous
celui à 48 kHz, sans écrêtage. Coût : 1,9 % CPU à 160 MHz, retard 0,8 ms.

**Export vers le PC : fait (2026-10-05).** À la réception de `S` sur l'USB-Serial/JTAG, le firmware
coupe ses logs et envoie le flux 16 kHz en binaire (trames de 5 ms : magic `A5 5A C3 3C`,
séquence uint16, nombre d'échantillons uint16, puis int16 LE). `firmware_i2s_test/tools/capture_wav.py`
(uv + pyserial) ouvre le port sans DTR/RTS (sinon reset de la carte), envoie `S`, vérifie la
séquence et écrit un WAV. 10 s de musique : 2023 trames, 0 perte, 0 resynchronisation ; spectre
cohérent (rien de continu, contenu jusqu'à 7 kHz) ; **écoute jugée propre par l'utilisateur**.
Piège : `usb_serial_jtag_driver_install()` refuse un buffer RX ≤ 64 octets.

**Phase 3 : TERMINÉE (2026-10-05).** Critère de sortie atteint : flux PCM 16 kHz mono propre, extrait
et vérifié hors BLE, réécouté sur PC.

- Câblage et configuration de l'ADC I2S externe choisi (cf. architecture §2.2).
- Capture I2S continue, vérification du signal (par ex. dump vers fichier/UART pour écoute externe,
  ou mesure au niveau du signal numérique).
- Downmix stéréo→mono, rééchantillonnage vers 16 kHz.
- Détection de niveau de signal (pour la logique start/stop automatique de la Phase 6).

**Critère de sortie** : flux PCM 16 kHz mono propre et sans artefact, extrait et vérifié hors BLE
(ex. renvoyé sur UART/USB vers le PC et rejoué pour écoute).

## Phase 4 — Encodeur G.722

**Objectif** : intégrer et valider l'encodeur.

- Choisir une implémentation G.722 existante (licence permissive, empreinte mémoire raisonnable) —
  comparer 2-3 options avant de figer.
- Intégrer au pipeline, découpage en trames de 160 octets / 20 ms.
- Validation : encoder un signal connu et comparer visuellement/à l'oreille avec un encodeur de
  référence (ex. ffmpeg installé sur ce PC, `ffmpeg -i test.wav -ar 16000 -ac 1 -acodec g722 out.g722`)
  pour s'assurer que le format produit est correct avant de l'envoyer à une vraie aide.

**Critère de sortie** : trames G.722 générées par le firmware, bit-comparables (ou perceptuellement
équivalentes) à une référence logicielle connue.

**Statut : TERMINÉE (2026-10-05).**

- **Choix** : encodeur SpanDSP (Steve Underwood), dans sa version maintenue **sippy/libg722**
  (commit `4c2e79c`), copié dans `firmware_i2s_test/components/g722/` (voir `VENDOR.md`).
  Licence : domaine public + code CMU 1993 à usage libre. C'est la même lignée que l'encodeur de la
  pile Bluetooth d'Android (AOSP `packages/modules/Bluetooth/system/embdrv/g722`, utilisé pour
  l'ASHA), avec le même empaquetage `(bande haute << 6) | bande basse`. Écartés : ffmpeg/libavcodec
  (LGPL, infrastructure lourde), code de référence ITU (licence floue, plus lourd).
- **Bit-exactitude sur PC** : libg722 (bindings Python `G722`) et l'encodeur g722 de ffmpeg
  produisent des octets identiques sur 10 s de musique réelle (80 920 octets, 0 différence) — or
  c'est ffmpeg qui avait encodé la tonalité acceptée par l'aide en Phase 2.
- **Bit-exactitude sur la cible** : auto-test au démarrage, signal entier déterministe (triangle +
  bruit LCG) encodé sur l'ESP32, CRC32 `0x3705e003` identique à la référence PC. (Piège : `//` de
  Python arrondit vers le bas, la division entière C tronque — utiliser `>> 2` côté C.)
- **Coût** : 12 % d'un cœur à 160 MHz pour un flux (≈ 8 % à 240 MHz). À garder en tête pour la
  Phase 6 si les deux aides reçoivent des flux distincts (deux encodeurs).
- **Temps réel** : 320 échantillons à 16 kHz → une trame de 160 octets toutes les 20 ms. Mode de
  flux `G` sur l'USB-Serial/JTAG ; `tools/capture_wav.py ... g722` enregistre le `.g722` et le
  décode avec ffmpeg. 10 s : 511 trames, 0 perte ; **écoute validée par l'utilisateur**.
  (Le champ « taille » de l'en-tête des trames compte désormais des octets, dans les deux modes.)

## Phase 5 — Intégration complète, un seul canal d'abord

**Objectif** : brancher audio réel → encodeur → BLE, sur une seule aide, en continu.

- Remplacer les trames factices de la Phase 2 par le vrai flux encodé de la Phase 4.
- Tenir la cadence 20 ms en continu (plusieurs minutes) sans dérive ni sous/sur-alimentation du
  buffer de trames.
- Écoute réelle côté aide : qualité, latence perçue, absence de coupures.

**Critère de sortie** : audio TV entendu clairement et en continu sur une aide, qualité jugée
acceptable par l'utilisateur.

### Avancement (2026-10-05)

Chaîne intégrée dans `firmware/` : `audio_capture.c` (I2S esclave 48 kHz → FIR 48→16 kHz → G.722,
cœur 1) alimente une file de trames ; `asha_audio_stream_task` les envoie sur le canal CoC. **Son
continu et propre entendu dans l'aide** (source téléphone, breakout éloigné de l'antenne).

Problèmes trouvés et corrigés en route :

1. **`BLE_HS_ESTALLED` mal traité** (cause des « 1 s de son toutes les 5 s ») : dans NimBLE,
   ESTALLED veut dire *trame acceptée*, envoyée dès que l'aide rend des crédits
   (`ble_l2cap_coc_continue_tx`). L'ancien code la renvoyait (→ EBUSY) puis la libérait alors que
   NimBLE la détenait encore → canal corrompu, réouvert en boucle. Corrigé : après ESTALLED, attendre
   `BLE_L2CAP_EVENT_COC_TX_UNSTALLED` (sémaphore) avant la trame suivante ; canal déclaré bloqué
   seulement après 1 s sans crédit.
2. **Ne jamais jeter une trame G.722 encodée** : le codage est différentiel, le décodeur de l'aide
   divergerait (le numéro de séquence restant continu, elle n'est pas prévenue). Si la file (6
   trames = 120 ms) est pleine, on saute 20 ms de PCM *avant* l'encodage.
3. **Scan figé quand personne ne lit l'USB** : le log du scan (1 ligne / 20 annonces) bloquait la
   tâche hôte NimBLE sur l'écriture console. Log ramené à 1 ligne / 1000 annonces.
4. **Reconnexion après perte de lien** : la liste blanche ne retrouvait pas l'aide après un
   éteindre/rallumer → retour au scan actif (la liste blanche reste utilisée au démarrage, où elle
   fonctionne).
5. **Brouillage radio par le breakout** : breakout + nappe près de l'antenne du Supermini → ~500
   attentes de crédits en 45 s et coupures ; éloigné à 30 cm → aucune. Harmoniques du quartz
   24,576 MHz (98e–100e dans la bande BLE) et de BCK 3,072 MHz.
6. **Ronflement = boucle de masse par le PC** : en pause, -54 dBFS pendant le flux contre -66 sans
   flux (source PC), et **-69 dBFS avec un téléphone sur batterie comme source**. Les rafales radio
   toutes les 20 ms (50 Hz) font circuler un courant dans la masse USB, qui s'ajoute au signal du
   jack référencé au PC. Ni le firmware ni le breakout en cause (ancien firmware testé : même
   ronflement). **À traiter pour la TV** : alimentation USB indépendante non reliée à la terre,
   isolateur de boucle de masse / transformateur audio, ou entrée optique.
7. **Supermini n°1 (MAC FC:01:2C:C3:74:64) défectueux** : 64,9 °C stabilisé seul sur USB contre
   52,5 °C pour le n°2 (même puce 4 Mo + PSRAM 2 Mo). Cause probable : conflits de bus des premiers
   essais Phase 3 (l'ESP32 pilotait MCLK/BCK/LRCK contre le PCM1808 maître). Remplacé par le n°2
   (MAC 50:78:7D:19:B7:24, re-appairé avec l'aide). Tâche de log de la température puce (5 s)
   ajoutée au firmware.

Reste pour clore la Phase 5 : test continu de plusieurs minutes validé par l'écoute, puis essai
avec la vraie source TV (en traitant la boucle de masse).

### Validation sur une seconde paire d'aides (2026-10-09)

Objectif : vérifier que le firmware n'est pas calé sur la paire principale. Option de compilation
`ASHA_TARGET_PAIR` dans `main.c` (1 = paire principale, 2 = paire secondaire, **défaut 1**) ;
à 2, pas d'auto-connect au démarrage vers les bonds existants (scan + filtre HiSyncId).

- **Identification** (sniffer en scan + log ESP32, off/on aide par aide) : annonce ASHA
  `01 02 34 44 43 31` (gauche) / `01 03 34 44 43 31` (droite), HiSyncId complet
  `9502464234444331`, codec G.722 (`codecs=0x0002`). Adresses **privées tournantes** (changent
  toutes les 1-2 min : `70:d8:18:02:e8:bf` -> `4c:7f:83:b4:ac:02` en cours d'annonce) — seul le
  HiSyncId identifie la paire, ne jamais filtrer par adresse.
- **Droite puis gauche : connexion, bonding Just Works, CoC (PSM 0x0080, peer_mtu 248), START
  acquitté et son entendu, du premier coup** — aucune adaptation protocolaire nécessaire.
- Parasites entendus = rafales d'attentes de crédits CoC + trames PCM sautées (liaison faible) ;
  coupure vers 4 m.
- **Bug trouvé et corrigé (commit `51a36a7`)** : à la perte de lien, `COC_DISCONNECTED` (tâche
  hôte) remettait `coc_chan` à NULL pendant un `ble_l2cap_send()` de la tâche audio -> Guru
  Meditation `LoadProhibited` (EXCVADDR 0x38 = `&chan->coc_tx`) et reboot ; l'aide ne
  ré-annonçant pas, aucune reconnexion possible. Mutex récursif autour de chaque envoi et de la
  remise à NULL. **Pas encore vérifié en direct** (pas de nouvelle perte de lien après le flash).
- Gel de la sortie USB de l'ESP32 observé une fois (plus rien sur le port, esptool ne répond
  plus, radio toujours active) -> RESET manuel. Cause non établie ; un reboot spontané plus tôt
  dans la session relève peut-être du bug ci-dessus.
- **Bonds NVS** : `CONFIG_BT_NIMBLE_MAX_BONDS=3` et l'ESP32 s'est appairé aux deux aides de
  la paire secondaire -> le bond de la paire principale a probablement été évincé, et au démarrage (cible 1)
  l'auto-connect par liste blanche prend `bonded_peers[0]`, qui peut maintenant être une aide de
  la paire secondaire. **Au prochain essai avec la paire principale : effacer la flash (`pio run -t erase`)
  avant l'upload**, puis laisser le premier contact se faire par scan + HiSyncId (re-appairage
  automatique).

## Phase 6 — Duplication sur les deux oreillettes + orchestration

**Objectif** : le système complet tel que décrit dans l'architecture.

- Étendre à la deuxième connexion/canal CoC, envoi synchronisé (même tick 20 ms).
- State machine complète : détection de signal TV → START automatique ; silence prolongé → STOP
  automatique ; gestion de la perte de connexion BLE avec reconnexion et reprise du stream.
- Test d'usage réel prolongé (heures) devant la TV.

**Critère de sortie** : usage quotidien fonctionnel sans intervention manuelle — allumer la TV
suffit à démarrer le relai, l'éteindre l'arrête, une coupure BLE ponctuelle se rétablit seule.

### Stéréo sur la paire principale — validée (2026-10-10)

Firmware : commits `69ed8f9` (capture stéréo), `8cef80f` (contexte par côté, orchestration),
`461765d` (robustesse). Flash effacée avant le flash (bonds repartis de zéro).

- **Architecture retenue** (modèle `asha_pipewire_sink`) : un contexte par côté, aiguillage des
  notifications par `conn_handle` (les deux aides ont les mêmes handles GATT). Toute arrivée ou
  départ d'une aide pendant le flux → STOP des côtés actifs, reset des deux encodeurs, seq = 3,
  `03 01`/`03 00` + START (`otherstate` = 1 si deux aides) sur chaque côté. Une tâche d'envoi
  permanente pousse la même seq aux deux canaux ; une aide sans crédits bloque la paire (pas de
  trame jetée d'un seul côté, le décodeur G.722 est différentiel).
- **Déroulé** : gauche connectée et en flux mono en 6 s ; droite trouvée par le scan lent pendant
  le flux, prête 4 s après, redémarrage du groupe en ~0,1 s (2 trames jetées en tout), START
  acquitté des deux côtés. **Stéréo correcte à l'écoute**, liaison stable 13 min à 15 cm,
  0 trame jetée après le redémarrage, file ≤ 1/6, ~100 attentes de crédits par côté, puce 60 °C.
- **Piège : téléphone connecté aux aides.** Premier essai refusé par l'aide (`LE COC error: 6`,
  Insufficient Authorization) alors que l'appairage avait réussi ; Bluetooth du téléphone coupé →
  succès immédiat. L'aide réserve vraisemblablement son canal audio au téléphone connecté. C'est
  probablement aussi une explication des `LE COC error: 6` « inexpliqués » de septembre.
- **Reset après flash** : le reset RTS laisse la carte en mode téléchargement ; utiliser
  `esptool --after watchdog-reset` (ou bouton RESET).
- **Non résolu** : deux paniques `Interrupt wdt timeout` (les deux cœurs en idle) à 07:31:46 et
  07:32:07, pendant le scan, juste après l'échec CoC et le premier bond enregistré ; plus rien
  ensuite (20 min). Cause inconnue, à surveiller.
- **Essai +20 dBm (même matinée), abandonné** : la distance de décrochage n'augmente pas par
  rapport à +9 dBm (le lien est vraisemblablement limité dans le sens aide → ESP32). Derrière un
  isolateur USB, +20 dBm provoque des redémarrages juste après START et un fort ronflement
  (alimentation insuffisante pour les pointes d'émission). Retour à +9 dBm.
- **Droite éloignée → plus de son à gauche** (en stéréo) ; retour par off-on des deux aides. Cause
  non établie : la console USB-JTAG s'est figée 14 s après le START (sans panique ni redémarrage),
  rien n'a été enregistré. Hypothèse à vérifier : lien droit à moitié vivant, boucle
  blocage 1 s → réouverture CoC → redémarrage du groupe (STOP à gauche). Prochain essai avec la
  console UART0 (GPIO43) via un adaptateur USB-série derrière l'isolateur, ESP32 sur alimentation
  externe.
- **Reste à tester** : perte de portée d'un côté puis retour (chemin « départ d'une aide »),
  portée en stéréo, correctif du crash à la déconnexion.

### Migration sur XIAO ESP32-S3 + Seeed XIAO Debug Mate (2026-10-11)

But : antenne externe (prise IPEX) pour la portée, console UART0 lisible sans dépendre de
l'USB-JTAG, alimentation externe 5 V possible.

- **Carte vérifiée** (`esptool flash-id`) : ESP32-S3 QFN56 v0.2, flash 8 MB (quad), PSRAM 8 MB
  embarquée. Env PlatformIO `xiao_esp32s3` (preset `seeed_xiao_esp32s3`) ; `sdkconfig` généré
  identique à `esp32s3_com12` hormis la taille flash. Flash effacée avant le premier flash.
- **Câblage, mêmes GPIO que la Supermini → aucun changement de code** : BCK = D4 (GPIO5),
  LRCK = D5 (GPIO6), DATA = D8 (GPIO7), MCLK non connecté, PCM1808 en 3V3/GND, console UART0
  TX = D6 (GPIO43).
- **Debug Mate** : réglage série **XIAO** (écoute D6/D7 de la XIAO enfichée, sans fil) en mode
  **Passthrough**, 115200 bauds ; il apparaît côté PC comme port série DAPLink (VID 0D28). Le
  mode GROVE sert à un appareil UART externe sur le connecteur Grove. Attention à ne pas mettre
  en parallèle l'alimentation externe 5 V, le 5 V du Debug Mate et le VBUS USB de la XIAO.
- **Acquisition validée sans les aides** (log `audio:` toutes les 10 s, source PC) : 50 trames/s,
  0 erreur I2S, 0 trame jetée ; entrée débranchée −53 dBFS (entrée flottante), source branchée
  volume 0 **−65 dBFS** (−54 dBFS sur la Supermini dans les mêmes conditions), musique volume 30
  **−42 dBFS** sur les deux voies.
- **Essai avec les aides (paire principale, flash effacée)** : appairage, CoC et START
  acquittés des deux côtés du premier coup, mais **aucun son** au premier flux après l'appairage
  neuf (bip d'appairage entendu). Off/on des deux aides → son dans les deux aides. À retenir :
  après un appairage neuf, éteindre et rallumer les aides avant de conclure.
- **Debug Mate en Passthrough** : ne transmet au PC que si le port est ouvert avec **DTR
  actif** (PuTTY / moniteur PlatformIO le font ; pyserial : `dtr=True`). Le flux perd des
  fragments de lignes et arrive avec ~10 s de retard, mais il survit aux gels de la console
  USB-Serial/JTAG (encore observés à chaque redémarrage de la carte pendant que le port est ouvert).
- **Crash trouvé et corrigé** : `LoadProhibited` dans `ble_hs_conn_delete_chan` après une
  réouverture CoC en stéréo. Le canal était rouvert depuis le callback
  `BLE_L2CAP_EVENT_COC_DISCONNECTED`, quand NimBLE n'a pas encore libéré l'ancien canal : avec
  `L2CAP_COC_MAX_NUM=2` et deux aides, `ble_l2cap_connect()` échouait en `rc=6`
  (`BLE_HS_ENOMEM`), en appels récursifs, puis la déconnexion plantait dans NimBLE. Probablement
  aussi la cause du « droite éloignée → plus de son à gauche » du 2026-10-10. Correctif :
  réouverture différée par un callout NimBLE (tâche hôte, après libération de l'ancien canal,
  abandonnée si l'aide est déconnectée entre-temps), `L2CAP_COC_MAX_NUM` = 4, contrôle du
  tampon de réception et du code retour à l'ouverture. Validé en réel : aide tombée sans panique.
- **Appartenance au groupe** : une aide qui tombait pendant la réouverture de son CoC était en
  `SETUP` et n'était pas considérée comme membre du groupe → l'autre restait en stéréo (une
  seule voie) sans STATUS ni redémarrage mono. Indicateur explicite `in_group` posé quand l'aide
  est prête. Pas encore vérifié en réel.
- **RSSI du lien** de chaque aide connectée, journalisé toutes les 5 s (`rssi: aide G : -xx
  dBm`). Au scan, l'annonce des aides sort à −76/−92 dBm ; une fois connectées, −50 à −75 dBm.
- **Portée, antenne d'origine de la XIAO** (aides portées, paliers de 20 s) : sans défaut
  jusqu'à 3 m (RSSI −63 à −81 dBm, presque indépendant de la distance : l'orientation et le
  corps dominent), **coupure vers 4 m** (creux à −102 dBm côté droit, attentes de crédits des
  deux côtés, 13 trames sautées). La Supermini décrochait vers 1,5 m. Une minute après le retour,
  l'aide gauche a coupé d'elle-même (reason 531, RSSI correct, rien de bloqué côté ESP32) :
  cause inconnue, à capturer au sniffer si ça se reproduit.
- **Antenne de point d'accès récupérée (connecteur IPEX direct)** : aide vue à −91 dBm à courte
  distance, connexion perdue 1,4 s après l'établissement (reason 519) ; retour à l'antenne
  d'origine → reconnexion normale. Antenne à diagnostiquer.
- **Passage en mono validé en réel** : l'aide droite tombée pendant un essai de portée → STATUS
  et redémarrage du groupe en mono immédiats (indicateur `in_group`).
- **Reste à faire** : test d'un canal seul (inversion G/D).

### Coupures 531 et silences : un iPhone appairé aux aides (2026-10-11, après-midi)

Symptômes : aide qui coupe d'elle-même (`reason=531`) avec un bon RSSI, et périodes où l'aide
n'émet plus de son alors que l'ESP32 reste connecté, envoie le flux et reçoit des crédits.

**Méthode (réutilisable)** :
- Bonds NimBLE en RAM seulement (`NVS_PERSIST` désactivé) : rien à lire en flash. Option de
  compilation `ASHA_LOG_BOND_KEYS` (défaut 0, ex. `PLATFORMIO_BUILD_FLAGS="-DASHA_LOG_BOND_KEYS=1"`)
  : affiche l'identité, l'IRK et la LTK de chaque aide au chiffrement, et demande la distribution
  de l'identité (sans elle, pas d'IRK). Les clés valent jusqu'au prochain reset de l'ESP32.
- Sniffer : `nrfutil ble-sniffer sniff --port COMxx --irk-key 0x<IRK> --sc-ltk-key 0x<LTK>
  --output-pcap-file x.pcap` (clés en big-endian). Le sniffer s'accroche aux annonces de l'aide
  (adresses privées résolues) mais rate souvent le CONNECT_IND de l'ESP32. L'horloge des paquets
  du sniffer était décalée de +71 min 35 s ce jour-là.
- Résolution d'une adresse privée avec une IRK : `ah(irk, prand) = AES-128(irk, 0^13 || prand)`,
  3 derniers octets, clé en big-endian.
- Dans les paquets en clair au début d'une connexion, `LL_VERSION_IND` donne le fabricant de la
  puce et la version Bluetooth de chaque côté : c'est ce qui a désigné le coupable.

**Constats** :
- L'aide gauche **continue d'émettre ses annonces pendant qu'elle est connectée** à l'ESP32.
- Un autre central sollicitait l'aide gauche par rafales (jusqu'à une demande de connexion toutes
  les ~6 s) : connexion à 30 ms, `LL_VERSION_IND`, recherche de service, `LL_ENC_REQ` avec sa
  propre clé (déchiffrement « bad MIC » avec la nôtre), puis nouvelle tentative ; cinq adresses
  privées successives. Les rafales coïncidaient avec les coupures et les silences.
- Éliminés un par un (coupure du Bluetooth ou extinction, et par le RSSI) : les deux PC Windows,
  téléphones et tablette de la maison, télévision, aide droite, XIAO et Debug Mate.
- `LL_VERSION_IND` du central : **Broadcom, Bluetooth 5.3** (l'aide : Sivantos, 5.1) → piste Apple.
  Un **iPhone SE oublié en charge** après une décharge profonde s'était rallumé seul ; au
  déverrouillage (code demandé), les demandes ont cessé (16:09:38) et l'iPhone a affiché
  « connecté aux aides ». Hypothèse : avant le premier déverrouillage après un redémarrage, iOS
  n'a pas accès aux clés d'appairage et retente la connexion en boucle. iPhone éteint (appairage
  conservé à la demande de l'utilisateur).
- **À retenir** : tout appareil appairé aux aides (iPhone en MFi, application du fabricant sur un
  autre téléphone…) entre en concurrence avec l'ESP32, au point de couper le flux. S'il se
  rallume (en charge, par exemple), le problème reviendra.
- Reste à confirmer : un essai long (ESP32 + deux aides, 2,5–3 m, ~15 min) sans aucun
  appareil concurrent, pour vérifier que les coupures 531 et les silences ont disparu.

### Aides muettes après un creux radio : relance automatique du groupe (2026-10-11, soir)

Essais avec l'iPhone éteint et le sniffer en surveillance (aucun appareil étranger, hormis des
paquets isolés corrompus).

- **Antenne d'origine, 2,5–3 m, firmware sans relance** : une perte audible toutes les 1 à 2 min.
  Chaque perte correspond à un creux radio sur une aide (−90 à −114 dBm, même immobile : passages,
  réflexions). Souvent **les deux aides deviennent muettes**, et y restent, alors que les liaisons
  sont revenues, que les trames leur parviennent et qu'elles rendent leurs crédits ; aucune
  notification d'état. Le côté qu'on entend couper n'est pas forcément celui dont le lien a faibli
  (synchronisation stéréo entre les aides, hypothèse). Au bout de 40 s à 4 min, une aide finit par
  **couper elle-même la liaison (531)** avec un bon RSSI : c'est l'autre cause des 531.
- **Codes de déconnexion** : extinction d'une aide par son bouton → 520 (perte de liaison, 3 s) ;
  531 = l'aide a décidé de couper.
- **Vérification** : la coupure 531 de la gauche a provoqué un redémarrage du groupe en mono, et
  la droite, muette jusque-là, a retrouvé le son → un STOP/START débloque une aide muette.
- **Correctif** (commits `74252ca`, `c99f2d7`) : `asha_group_resync()` (STOP puis START des aides
  en flux) ; relance automatique quand une liaison est passée sous −92 dBm puis que toutes sont
  revenues au-dessus de −85 dBm (RSSI relevé toutes les 5 s), au plus une fois toutes les 10 s ;
  relance manuelle par le bouton BOOT (GPIO0). Validé : à 4 m puis 5 m le son se coupe, et la
  stéréo revient seule en se rapprochant, sans extinction des aides ni coupure 531.
- **Autre bug corrigé** (`74252ca`) : une connexion qui tombe avant `BLE_GAP_EVENT_CONNECT`
  (reason 519, avec `BLE_ERR_UNK_CONN_ID` sur la lecture de version) laissait `g_connecting` à vrai
  → scan jamais relancé, la seconde aide ne revenait pas.
- **Antenne RP-TNC de 10 cm** (routeur Wi-Fi, via adaptateur) : RSSI comparable mais ~550
  attentes de crédits en 1 min 30 près de la carte (0 avec l'antenne d'origine) et coupures même
  de près → connecteur ou adaptateur suspect, à passer au SNA D6 (`diagnostic_antenne.md`).
- **Pistes** : relever le RSSI plus souvent (1–2 s) pour relancer plus tôt ; déclencher aussi sur
  une rafale d'attentes de crédits ; essai long en usage réel (TV, 2–3 m, 30 min).
- **Coût d'une relance** (bouton BOOT, près de la carte) : ~0,5 s de son perdu ; l'aide remonte le
  son en douceur (~0,5 s) mais coupe net au STOP. Piste : fondu sortant de ~200 ms appliqué au PCM
  avant le STOP. Bug corrigé (`027606d`) : pile de 2 Ko trop petite pour la tâche du bouton
  (redémarrage de l'ESP32 à l'appui).

### Reconnexion sans off/on après un redémarrage de l'ESP32 (2026-10-11, soir)

Jusqu'ici, tout redémarrage de l'ESP32 (flash, coupure de l'USB, mise en veille du PC) imposait
d'éteindre et rallumer les aides : les bonds n'étaient qu'en RAM, il fallait refaire un appairage,
que les aides n'acceptent que peu après leur mise en marche.

Correctif (commit `2b26372`) :
- `CONFIG_BT_NIMBLE_NVS_PERSIST=y` : bonds enregistrés en flash ;
- distribution de l'identité (IRK) toujours demandée : le contrôleur résout les adresses privées
  tournantes des aides vers leur adresse d'identité (publique, `20:3a:ef:…`) ;
- au démarrage, la liste blanche existante (`ble_gap_wl_set` + connexion, 10 s) prend la première
  aide connue ; la seconde est trouvée par le scan, qui reconnaît maintenant une aide **déjà
  appairée** par son adresse d'identité, même sans données ASHA dans l'annonce. Le contrôleur
  signale l'adresse résolue avec le type `PUBLIC_ID` (2) : ramené à `PUBLIC` (0) pour retrouver le
  bond.

Vérifié : après un reset de l'ESP32, aides non touchées, droite connectée et chiffrée en 1,1 s,
stéréo en 5 s. Une aide déjà appairée s'annonce sous son adresse d'identité, et l'aide gauche
continue d'émettre ses annonces pendant qu'elle est connectée.

À retenir : `MAX_BONDS=3` (le plus récent évince le plus ancien) ; pour changer de paire d'aides
(`ASHA_TARGET_PAIR`), effacer la flash (`pio run -e xiao_esp32s3 -t erase`) puis appairer.

### Fin du repli en mono : canal toujours affecté à son aide (2026-10-17)

Changement de stratégie : le firmware ne gère plus le passage en mono quand une aide est absente
ou se déconnecte. Chaque voie du PCM1808 reste affectée à son aide (gauche → aide gauche, droite →
aide droite), le mélange (G+D)/2 n'est plus utilisé. Cela remplace le comportement « STATUS +
redémarrage du groupe en mono » décrit plus haut (validation du 2026-10-10/11).

- **Canaux** : `asha_start_all_ready()` active toujours la stéréo, même avec une seule aide prête.
  `ASHA_STEREO=0` reste possible à la compilation (mélange sur les deux aides).
- **Aide perdue** (`asha_on_side_lost()`) : l'autre aide reçoit le STATUS « autre côté
  déconnecté », puis est redémarrée seule (STOP/START, `otherstate=0`) sur sa propre voie.
  Première version (STATUS seul, flux intact) abandonnée : mesuré le 2026-10-17, l'aide restante
  devient muette dès que l'autre disparaît (extinction ou coupure 531, liaison pourtant active et
  crédits rendus) et ne revient qu'après un STOP/START ou son propre 531 (~10 s).
- **Aide qui revient pendant le flux de l'autre** : elle démarre seule (STATUS « connecté » à
  l'aide en flux, puis START `otherstate=1` sur la nouvelle), sans STOP/START du groupe. La séquence
  commune `g_seq` n'est pas remise à 3 (les deux aides se calent dessus). À l'ack du START, seul
  l'encodeur G.722 de la voie de l'aide qui rejoint est remis à zéro
  (`audio_capture_reset_channel()`), son décodeur partant à vide ; l'autre voie n'est pas touchée.
- **Inchangé** : le premier démarrage (aucune aide en flux) et la relance (`asha_group_resync()`,
  bouton BOOT) arrêtent et relancent toujours le groupe, séquence repartie à 3.
- **À vérifier en réel** : que la Signia accepte un START avec une séquence en cours (≠ 3) et
  qu'elle reste calée avec l'autre aide ; continuité du son à proximité, puis essais de distance
  avec déconnexion et reconnexion d'une aide. Si le START tardif est refusé ou désynchronisé, le
  repli est le redémarrage commun du groupe à l'arrivée d'une aide (ancien comportement de
  `asha_group_update()`).

#### Relance automatique rendue moins fréquente (2026-10-17)

Écoute à moins de 0,5 m, antenne d'origine, sans bouger (3 min de traces,
`sniffs/esp32_serial_11.log`) : capture saine (`jetees` stable, 0 erreur I2S), mais le RSSI
relevé toutes les 5 s plongeait à −95/−104 dBm environ toutes les 30 s, sans perte de son
réelle, et la relance automatique se déclenchait toutes les ~60 s (coupure de ~0,5 s puis retour
progressif du son, reconnu à l'écoute). Environ 1 à 2 attentes de crédits CoC par seconde et par
aide.

Correctif : un creux ne compte que s'il dure `ASHA_RESYNC_DIP_SAMPLES` (8) mesures consécutives
sous −92 dBm ; écart minimal entre deux relances automatiques porté à 180 s (maximum 300 s,
retour au minimum après 600 s de calme). Le bouton BOOT reste disponible, et
`ASHA_AUTO_RESYNC=0` la désactive. À surveiller : une aide restée muette après un vrai creux
doit encore être relancée, avec ce délai de réaction plus long.

#### Premier essai d'éloignement : gauche muette après le retour de la droite (2026-10-17)

Traces : `sniffs/esp32_serial_12.log`. La droite descend à −101/−107 dBm avec une rafale
d'attentes de crédits (151), 25 trames sautées côté ESP32, puis coupe d'elle-même (531) après
~50 s. La gauche (−86 dBm au plus bas, 51 attentes de crédits) reste connectée et en flux pour
l'ESP32, mais **reste muette** d'après l'écoute, y compris après le retour de la droite (3 s,
démarrage seule correct). Avant la fin du repli en mono, la coupure 531 relançait le groupe, ce
qui réveillait l'autre aide : le démarrage seul ne le fait plus.

Correctif : chaque seconde, le suivi RSSI marque une liaison « en difficulté » pour 60 s si son
RSSI est ≤ −85 dBm ou si elle a eu ≥ 10 attentes de crédits dans la seconde (régime normal : 1 à
2 par seconde). Quand une aide revient et qu'une aide en flux est marquée, le groupe est relancé
(fondu, STOP, START des deux) ; sinon l'aide rejoint seule, sans trou sur l'autre. À valider en
réel : gauche audible après un retour de la droite, et absence de coupure à la reconnexion au
proche. **Validé le 2026-10-17** : extinction puis rallumage de chaque aide, l'autre repart seule
en ~1 s à la perte et continue sans interruption au retour de la première ; le correctif
d'éloignement (relance si l'autre a souffert) a réveillé la gauche muette.

#### Régime de connexion, durée d'événement Android et limite de portée (2026-10-17, après-midi)

- **Mémoire** (ligne `heap:` toutes les 5 s) : tas libre stable (~203 Ko), minimum figé, plus gros bloc
  constant (155 648 octets), `mbuf` CoC 14-16/16 : ni fuite ni fragmentation sur ~15 min. Pas de lien
  avec la durée de fonctionnement.
- **Le régime des attentes de crédits dépend de la connexion, pas du RSSI** : sur plusieurs captures,
  0,1-0,3 attente/s pendant des minutes sur une connexion, 3-40/s sur une autre au même RSSI (−65 dBm),
  qui change à la (re)connexion. Les creux simultanés sur les deux aides laissent les deux muettes
  malgré une liaison active : la relance automatique les prend maintenant en compte (un seul
  relevé sous −88 dBm sur toutes les aides en flux suffit).
- **Code Android** (`hearing_aid.cc`, `UpdateBleConnParams`) : pour 20 ms, intervalle 0x10, latence
  0x0A, timeout de supervision 0x64, et **`min_ce_len = max_ce_len = 0x000C`** (7,5 ms). Notre
  firmware demandait 0/0 : le contrôleur ne réservait pas de temps radio par liaison. `ASHA_CE_LEN`
  = 0x000C appliqué à la demande initiale et au re-tirage. Après ce changement : régime à 0 attente
  de crédits avant et après une coupure/reconnexion, contre 3-8/s sur l'essai précédent. Un seul
  cycle, causalité à confirmer. Timeout de supervision conservé à 3 s (Android : 1 s).
- **Re-tirage** : attentes de crédits lissées ≥ 1,5/s avec RSSI > −80 dBm → renvoi des paramètres de
  connexion (latence alternée 10/9), au plus une fois par minute et par aide. Seuil à resserrer vers
  −72 dBm (re-tirages inutiles à signal faible, sans effet vu).
- **Tampon d'envoi** porté de 6 à 25 trames (500 ms, `QUEUE_DEPTH`) : absorbe les attentes de crédits
  sans jeter de PCM (jetées stables à 80-83). **À ramener vers 6 (120 ms) avant de brancher la
  télévision** (latence image/son).
- **Traces ajoutées** : niveau de sortie après fondu et gain (`sortie G/D`, `gain`) pour distinguer un
  fondu coincé à zéro d'un silence côté aides.
- **Portée** : limite à ~4 m (antenne d'origine de la carte), creux à −100/−109 dBm sur les deux
  aides, coupures brèves puis reconnexion seulement au retour. Puissance d'émission laissée à
  +9 dBm (décision : pas d'augmentation) ; le RSSI relevé est celui du sens aide → carte, hors de
  portée d'un réglage côté carte. Piste retenue : travail sur l'antenne.

- **Relance sur période de liaison faible** : essai à 4 m, liaison faible pendant ~35 s (D −72 à
  −100 dBm, G jusqu'à −87), puis ~90 s de silence aides connectées avec liaison revenue à −60 dBm,
  sans relance (creux espacés, jamais simultanés). Nouveau déclencheur : score à décroissance 0,9/s
  alimenté quand une aide en flux est à ≤ −80 dBm, seuil 4 (~5 s faibles de suite ou ~8 sur 12) ;
  relance au retour du signal (3 s au-dessus de −85 dBm), 180 s minimum entre relances. Retour
  utilisateur : connexion et surtout reconnexion nettement plus immédiates, meilleur confort.

## Phase 7 — Finition (optionnelle, selon retour d'usage)

- Réglage de volume physique : encodeur rotatif + LED (voir « Interface utilisateur » ci-dessous).
- Boîtier, alimentation définitive, câblage propre côté prise TV.
- Documentation utilisateur courte si le boîtier doit être manipulé par quelqu'un d'autre que
  l'utilisateur développeur.

### Interface utilisateur : encodeur rotatif et LED (spécification du 2026-10-17, à implémenter)

Une seule commande : un encodeur rotatif (volume) et une LED. Elle pilote aussi les demandes de
connexion, donc le Bluetooth peut être entièrement éteint pour laisser le téléphone de l'utilisateur
libre de se connecter à ses aides.

**Comportement attendu**
- **Démarrage** : pas de Bluetooth. La LED clignote 0,2 s toutes les 2 s.
- **Un cran vers la droite** : LED allumée fixe, le Bluetooth démarre et les demandes de connexion
  sont émises (l'utilisateur est supposé proche). Le volume part du minimum et augmente à chaque
  cran vers la droite.
- **Volume au maximum** : clignotement très rapide pendant 2 s (0,2 s toutes les 0,5 s), puis LED
  allumée fixe.
- **Vers la gauche** : le volume diminue. Au-delà du minimum, l'appareil déconnecte les aides et
  éteint le Bluetooth (retour à l'état de démarrage).

```plantuml
@startuml
[*] --> Veille
state "Veille
Bluetooth éteint
LED 0,2 s / 2 s" as Veille
state "Actif
Bluetooth + connexions
LED fixe" as Actif
state "Volume maximum
LED 0,2 s / 0,5 s pendant 2 s" as Max
Veille --> Actif : cran à droite
(volume = minimum)
Actif --> Actif : cran : volume +/-
Actif --> Max : volume atteint le maximum
Max --> Actif : 2 s écoulées (LED fixe)
Actif --> Veille : cran à gauche
au-delà du minimum
(déconnexion, Bluetooth éteint)
Max --> Veille : cran à gauche
au-delà du minimum
@enduml
```

**Câblage proposé** (numéros de GPIO ; noms de broche de la XIAO ESP32S3 entre parenthèses).
Hors broches déjà utilisées : I2S du PCM1808 (GPIO5, 6, 7), bouton BOOT (GPIO0), console
(GPIO43/44). GPIO4 évité (diaphonie avec BCK constatée en phase 3), GPIO3 évité (broche de
configuration au démarrage).

| Fonction | GPIO | Broche XIAO | Branchement |
|---|---|---|---|
| Encodeur A (CLK) | GPIO1 | D0 | borne A |
| Encodeur B (DT) | GPIO2 | D1 | borne B |
| Encodeur bouton (SW) | GPIO9 | D10 | optionnel, réservé |
| Commun de l'encodeur | GND | GND | borne C |
| LED | GPIO8 | D9 | GPIO8 → 330 Ω → anode, cathode → GND (allumage actif haut) |

- Encodeur nu : résistances de rappel internes activées, plus 10 nF entre A (puis B) et GND contre
  les rebonds. Module de type KY-040 : alimenter en **3,3 V**, jamais en 5 V.
- Lecture de préférence par le compteur matériel PCNT (filtre anti-rebond intégré).
- Garder les fils de l'encodeur à l'écart des fils de l'I2S (horloge de 6 MHz).
- LED : ~4 mA en rouge avec 330 Ω sous 3,3 V ; 220 Ω pour une LED bleue ou blanche.

**Implémenté (2026-10-17)**, validé au banc : `ui_task` dans `firmware/main/main.c` (test matériel
isolé dans `test_ui/`).
- Volume : 16 crans de -100 dB (minimum) à 0 dB (ASHA_VOL_STEPS, ASHA_VOL_MIN_DB), écrit sur les
  aides connectées à chaque cran et repris dans le START. Il repart au minimum à chaque mise en
  route du Bluetooth, sans mémorisation.
- LED : fixe = les deux aides connectées ; clignotement rapide = Bluetooth en fonction et
  recherche en cours (ou volume maximum atteint, pendant 2 s) ; flash lent = veille.
- « Bluetooth éteint » = arrêt logiciel du scan et des connexions (aides déconnectées) ; le
  contrôleur reste initialisé, pour éviter un redémarrage de NimBLE.
- Le bouton BOOT garde la relance manuelle du groupe.
- Bouton de l'encodeur (GPIO9, D10, actif bas) : change de paire. Bluetooth arrêté, puis 1 flash de
  LED = paire principale, 2 flashs = paire secondaire (par défaut). Le choix est mémorisé en NVS
  (espace "ui", clé "pair"). Compilé le 2026-10-17, à valider au banc.

---

## Outils de validation transverses

- **nRF Connect** (déjà utilisé) : vérification manuelle ponctuelle de l'état GATT réel des aides.
- **Sniff HCI Android** (déjà en main, méthode documentée dans la conversation) : à refaire si un
  comportement du firmware diverge de ce qui est attendu, pour comparer trame par trame avec la
  référence Android.
- **Sniff BLE côté ESP32** : envisager un sniffer BLE externe (nRF52840 dongle en mode sniffer +
  Wireshark, ou équivalent) si un doute apparaît côté firmware sur ce qui est effectivement émis à
  l'antenne — plus fiable qu'un simple log logiciel pour diagnostiquer un problème de timing/crédit
  L2CAP.

### Sniffer nRF52840 — opérationnel (2026-09-23)

**Statut : outil prêt à l'emploi, pas encore utilisé pour une vraie session de capture Phase 2.**

Matériel : clones nRF52840 génériques ("SuperMini NRF52840", compatibles nice!nano,
bootloader UF2 Adafruit, VID/PID bootloader `239A:00B3`). Deux exemplaires en main.

**Chaîne d'outils installée en portable** (aucune installation admin/UAC, tout sous
`%USERPROFILE%\softs_portable\`) :
- `nrfutil\` — CLI Nordic (`nrfutil.exe`) + plugins `ble-sniffer` (capture, écrit directement en
  `.pcap`, pilotable 100% en ligne de commande, pas besoin de Wireshark) et `device`. Le firmware
  officiel (`.hex`) est fourni tel quel dans
  `nrfutil\home\share\nrfutil-ble-sniffer\firmware\`.
- `jlink_portable\` — `JLink.exe` + `JLinkARM.dll` de SEGGER, extraits de l'installeur officiel via
  7-Zip sans l'exécuter (l'installeur NSIS se laisse extraire directement). Nécessaire car le clone
  J-Link utilisé (VID SEGGER `1366:0101`, firmware "ARM-OB STM32", S/N générique — clone qui imite
  le protocole SEGGER) a besoin de la vraie DLL SEGGER (`--jlink-dll` sous `nrfutil device`, ou
  directement `JLink.exe -CommanderScript <script>`).
- `wireshark_portable\` — `tshark.exe` seul, extrait de la même façon (pas d'installation, pas
  besoin de Npcap puisqu'on ne capture jamais depuis une carte réseau — uniquement pour relire/
  décoder les `.pcap` produits par `nrfutil ble-sniffer`).
- `uf2tools\` — `uf2conv.py` (Microsoft) + table des familles, utile si on repasse par le bootloader
  UF2 un jour (family ID nRF52840 = `0xADA52840`).

**Piège rencontré et résolu** : le firmware `sniffer_nrf52840dk_nrf52840_4.1.1.hex` (prévu pour la
vraie carte nRF52840-DK) est bare-metal, sans SoftDevice, table de vecteurs à l'adresse `0x0`. Flashé
naïvement par UF2 drag-and-drop sur le bootloader Adafruit (qui a lui aussi besoin de l'adresse `0x0`
pour son MBR/redirection double-reset), ça écrase le MBR et casse l'accès au bootloader — carte
inaccessible en USB (ni port série, ni lecteur bootloader), **mais pas brickée au niveau silicium**.
Récupérable par SWD. **Leçon : ce firmware sniffer ne doit être flashé QUE par SWD (J-Link/ST-Link),
jamais par UF2/drag-and-drop, sur ce type de carte.**

**Câblage SWD confirmé pour ce clone "SuperMini"** (pads au dos, sérigraphiés) :

| J-Link | Pad carte |
|---|---|
| VCC | VDD (mesuré 3.3V) |
| SWDIO | DIO |
| SWDCLK | CLK |
| GND | GND |

**Deux variantes du firmware Nordic testées** — la bonne dépend de l'architecture USB de la carte :
- `sniffer_nrf52840dk_nrf52840_4.1.1.hex` : suppose une sortie via une puce interface de debug
  externe comme sur la vraie DK. **Sur notre clone (USB natif, pas de puce interface séparée), ça
  fonctionne quand même en USB natif** — confirmé le 2026-09-23, énumère en `USB Serial Device`,
  VID/PID `1915:522A` (Nordic officiel). C'est cette variante qui est actuellement flashée et
  validée sur la carte fonctionnelle.
- `sniffer_nrf52840dongle_nrf52840_4.1.1.zip` (paquet DFU app-only, `.bin`+`.dat`, pas de
  SoftDevice/MBR inclus) : testée aussi (flashée en `.bin` brut à l'adresse `0x0` par SWD), n'a
  donné aucune énumération USB dans nos tests — non retenue pour l'instant, pas d'explication
  définitive trouvée.

**Procédure de flash validée** (`JLink.exe -CommanderScript <script.jlink>` avec un script
contenant `device NRF52840_XXAA` / `si SWD` / `speed 4000` / `erase` / `loadfile "<hex>"` / `r` /
`g` / `exit`) : efface toute la puce et reprogramme en ~3s, vérifié (Program & Verify OK).

**État des deux cartes physiques à la fin de la session** :
- **Carte fonctionnelle** (COM14 actuellement, le port change à chaque re-branchement) : firmware
  sniffer Nordic officiel flashé et **testé avec succès** — capture réelle de trafic BLE ambiant
  confirmée (`ADV_IND`/`ADV_NONCONN_IND` décodés correctement par tshark, ~131 Ko capturés en
  quelques secondes). Plus de bootloader UF2 dessus (normal, sacrifié volontairement — carte dédiée
  au sniffing désormais, reflashable uniquement par SWD).
- **Carte de secours** : intacte, toujours en bootloader UF2 Adafruit fonctionnel (`D:\NICENANO`),
  jamais touchée par un flash sniffer. Gardée en réserve.
- La carte qui a perdu son bootloader lors de l'essai raté initial (UF2 direct, cf. piège ci-dessus)
  est la carte devenue "Carte fonctionnelle" ci-dessus une fois recâblée en SWD — pas de carte
  perdue, juste un aller-retour.

**Commande de capture prête à l'emploi** (à lancer pendant une session de streaming Phase 2 réelle,
en ciblant l'adresse MAC de l'ESP32 ou de l'aide auditive) :
```
nrfutil.exe ble-sniffer sniff --port COM14 --follow <adresse MAC> --output-pcap-file capture.pcap
```
(le numéro de COM peut changer après un débranchement/rebranchement — revérifier avec
`Get-PnpDevice -Class Ports` avant chaque session).

**Prochaine étape** : lancer une vraie capture pendant un cycle START → streaming → STOP entre
l'ESP32 et l'aide Signia, pour vérifier trame par trame ce qui est réellement émis à l'antenne
(objectif initial de cette session, pas encore fait — l'essentiel du temps est passé à fiabiliser
l'outil lui-même).

### Session de durcissement Phase 2+ et test de portée (2026-09-24)

**Contexte** : reprise sur la carte S3 (COM12, confirmée par `esptool chip-id` — attention, l'utilisateur
avait cru par erreur que c'était le C3 en cours de session, revérifié et corrigé). Objectif : vérification
générale avant Phase 3+, avec le sniffer nRF52840 (COM14) et un test de portée réel à pied.

**Découverte 1 — firmware toujours "un seul cycle par connexion"** : le firmware ne relance jamais
START tout seul après un STOP sur la même connexion (conçu à l'origine pour un test unique). Sans
power-cycle de l'aide, aucune nouvelle tonalité n'est possible, même connexion GAP technique toujours
active.

**Découverte 2 — claquement de bouclage sur la tonalite de test** : sifflement 1kHz avec claquement
fort ~1x/seconde, causé par le bouclage d'un buffer G.722 d'1s (l'état interne adaptatif de
l'encodeur ne reconverge pas exactement au même point à chaque bouclage — vérifié empiriquement,
même après 500ms-1s d'établissement les octets encodés ne redeviennent jamais identiques d'un cycle
à l'autre, dérive numérique de l'implémentation ffmpeg). **Corrigé partiellement** : buffer étendu à
15s (750 trames), donc claquement espacé à 1x/15s au lieu de 1x/s. Pas de solution parfaite trouvée
sans réencoder — le vrai correctif est la Phase 3 (flux continu, plus de bouclage du tout).

**Découverte 3 — streaming rendu continu** : boucle de streaming modifiée pour tourner jusqu'à
déconnexion (`coc_chan != NULL`) au lieu de s'arrêter à 150 trames fixes — nécessaire pour un test de
portée à pied prolongé.

**Test de portée réel** : connexion tient jusqu'à ~1,5m, coupure au-delà. Comportement observé après
coupure : **jamais de reconnexion automatique en ramenant l'aide à portée**, il faut systématiquement
éteindre/rallumer l'aide.

**Découverte 4 — bug reel identifie et corrige** : `ble_l2cap_send()` retournait `rc=15`
(`BLE_HS_EBUSY`) de façon **soutenue et permanente** (des centaines de trames consécutives sur 45s+,
jamais de récupération même aide ramenée à portée) — crédits de flux L2CAP CoC épuisés et jamais
régénérés par l'aide après le premier accroc radio. Le lien GATT/ATT restait lui parfaitement
fonctionnel pendant ce temps (STOP réussissait toujours via écriture caractéristique), seul le canal
CoC audio spécifiquement restait mort. **Corrigé** : compteur d'échecs consécutifs (seuil 30, avec
retentative bornée à 100ms par trame) déclenchant une déconnexion GAP forcée
(`ble_gap_terminate()`) pour relancer un cycle de connexion propre avec des crédits frais — équivalent
logiciel du cycle d'alimentation manuel. **Testé en direct : le mécanisme se déclenche bien** (log
"canal CoC bloque — deconnexion forcee" après ~5s de blocage, `disconnect; reason=534` confirmé).

**Découverte 5 — limite réelle côté aide, pas côté firmware** : après la déconnexion forcée, le scan
reprend correctement (code ESP32 vérifié correct) mais **l'aide ne réémet plus jamais d'advertising
avec le HiSyncId ciblé** (85+ secondes observées sans réapparition). Comportement cohérent avec TOUTES
les observations de la session (aucune reconnexion automatique n'a jamais fonctionné, même avant les
correctifs) : l'aide semble arrêter son advertising général après une déconnexion normale
(économie de batterie probable) et n'accepte une nouvelle connexion qu'après un vrai cycle
d'alimentation. **Ce n'est pas un bug firmware ESP32 — c'est une limite de conception de l'aide**,
non contournable de notre côté sans un mécanisme de reconnexion différent (à creuser un jour via le
sniff HCI Android d'origine, si Android arrive à reconnecter sans intervention physique — pas encore
vérifié).

**Confirmation radio directe (sniffer, aide toujours proche et sous tension)** : scan d'advertising de
10s, 486 paquets captés (autres appareils BLE alentour), **zéro paquet des deux adresses connues de
l'aide** (`7a:18:a5:08:44:ba` capability 0x03 / `73:aa:0a:4a:f2:72` capability 0x02). Confirme sans
ambiguïté au niveau radio que l'aide n'émet plus RIEN en BLE après une déconnexion — pas un problème
de filtrage HiSyncId côté ESP32, pas un artefact du sniffer.

**Sniffer nRF52840 — limite supplémentaire découverte** : en mode `--follow` sur une longue durée
(ici ~50 minutes), le sniffer a cessé de capturer sans erreur visible ni crash — désync silencieux
du mode suivi après une longue période. À relancer périodiquement plutôt que de faire confiance à une
session `--follow` de longue durée.

**Suite et résolution (même session)** : idée décisive de l'utilisateur — avec un téléphone Android,
aucune manœuvre d'extinction/rallumage n'est jamais nécessaire, ni pour la connexion initiale ni après
une coupure. Recherche dans la doc officielle AOSP
([source.android.com/docs/core/connect/bluetooth/asha](https://source.android.com/docs/core/connect/bluetooth/asha))
: *"When the central isn't streaming audio data to the peripheral and can maintain a BLE connection,
the central shouldn't disconnect from the peripheral."* — le modèle attendu est **rester connecté en
permanence**, pas déconnecter/reconnecter à la demande.

Ça remettait en cause le correctif précédent (`ble_gap_terminate()` sur canal bloqué), qui allait à
l'encontre de ce principe. **Corrigé** (`f6421f4`) : sur canal CoC bloqué, on referme et rouvre
**uniquement le canal L2CAP CoC** (`ble_l2cap_disconnect()` + `ble_l2cap_connect()`), sans jamais
toucher à la connexion GAP sous-jacente — donc jamais besoin que l'aide ré-annonce.

**Testé en direct avec succès total** : cycle bloqué → fermeture/réouverture du canal → nouveau
START → reprise du streaming, répété plusieurs fois consécutives (toutes les ~2-5s selon les
conditions radio du moment), **sans aucune intervention manuelle, sans jamais perdre la connexion**.
Confirmé par l'utilisateur : "reconnexion automatique" fonctionnelle. La cause racine de
l'épuisement récurrent des crédits CoC (probablement lié aux conditions radio marginales du test)
n'a pas été investiguée plus avant, mais n'est plus bloquante : le firmware s'auto-répare de façon
fiable et transparente.

**Tentative de capture radio de la reconnexion Android** (pour comprendre le mécanisme exact) :
sniffer relancé en scan large pendant que l'utilisateur réactivait le Bluetooth de son téléphone.
Reconnexion Android confirmée immédiate côté utilisateur, mais **le sniffer n'a rien capté
d'exploitable** (ni les adresses connues de l'aide, ni service 0xFDF0, ni CONNECT_IND, sur ~14000
paquets/2min) — limite déjà connue du sniffer nRF52840 mono-radio face à un échange bref. Piste du
PlutoSDR de l'utilisateur évoquée pour une détection d'énergie RF simple (pas un décodage protocole
complet — bande ISM 2.4GHz de 83.5MHz trop large pour la bande passante native du Pluto ~20MHz) mais
pas retenue pour l'instant, la piste AOSP ayant suffi.

**Bilan final de la session** : 4 corrections firmware committées (`d421dac`, `f6421f4`) — tonalité
15s, retry EBUSY, récupération canal bloqué par réouverture CoC seule (pas de déconnexion GAP).
**Objectif de robustesse pleinement atteint** : streaming continu, auto-réparation transparente sur
incident radio, plus besoin d'aucune intervention manuelle sur l'aide. Prêt pour Phase 3.

**Piège d'environnement supplémentaire** : après un flash réussi (`esptool` confirme "Hash of data
verified"), le hard-reset automatique via RTS peut ne pas suffire à faire redémarrer proprement
l'application sur ce board S3 — la carte peut rester silencieuse sur le port série jusqu'à un appui
RESET manuel (déjà vu pour l'entrée en bootloader, confirmé ici aussi pour la sortie).

### Tentative de connexion initiale automatique après reset ESP32 (même session)

**Objectif** : reproduire le comportement Android — aucune manœuvre sur l'aide nécessaire, ni pour
la connexion initiale ni après une coupure, y compris après un reset/redémarrage du central.

**Implémenté** (non committé séparément, à committer si repris) : mécanisme d'auto-connexion par
liste blanche du contrôleur (`ble_gap_wl_set()` + `ble_gap_connect(..., NULL, ...)`), équivalent
NimBLE de `BluetoothDevice.connectGatt(..., autoConnect=true, ...)` sur Android — le contrôleur reste
armé en permanence sur l'adresse d'identité de l'aide (capturée via `peer_id_addr` à la première
connexion, ou retrouvée dans le bond store persistant `ble_store_util_bonded_peers()` au boot) et se
connecte dès la moindre annonce, sans scan actif côté hôte.

**Résultat des tests en direct** : un succès isolé (connexion en 22,7s après un reset, **sans**
toucher l'aide) s'est révélé être un faux positif — confirmé par l'utilisateur, c'était en fait la
fenêtre d'advertising encore ouverte suite à un rallumage manuel de l'aide fait *pour un autre test*
juste avant, pas une réémission spontanée. Tous les autres essais (plusieurs resets ESP32, aide non
touchée) ont échoué : scan actif de 80s+ sans jamais retrouver l'aide.

**Conclusion définitive** : confirme et renforce la découverte radio du début de session (scan
sniffer dédié, zéro paquet de l'aide) — **l'aide n'émet strictement aucun advertising tant qu'elle
n'est pas physiquement rallumée**, quel que soit le mécanisme utilisé côté central (scan actif ou
liste blanche/auto-connect) puisque les deux nécessitent que le périphérique transmette quelque
chose. Non réparable côté firmware ESP32. Android bénéficie du même problème mais l'évite la
plupart du temps grâce à sa philosophie "ne jamais se déconnecter tant que c'est possible" (voir
plus haut) — le cas d'une vraie déconnexion complète reste probablement aussi bloquant pour lui,
simplement plus rare en usage normal.

**Ce qui reste acquis et solide** : tant que la connexion GAP n'est jamais réellement coupée (grâce
au correctif de réouverture du seul canal CoC, `f6421f4`), ce problème ne se pose jamais — c'est la
vraie solution pratique. Le mécanisme de liste blanche reste dans le code (ne nuit pas, capture
correctement l'adresse d'identité et bond persistant) mais ne garantit pas la reconnexion après une
vraie perte de connexion complète (reset ESP32, perte de portée prolongée) — dans ces cas, un
rallumage manuel de l'aide reste nécessaire, comme (probablement) pour Android dans les mêmes
circonstances.
