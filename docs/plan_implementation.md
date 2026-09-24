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

## Phase 5 — Intégration complète, un seul canal d'abord

**Objectif** : brancher audio réel → encodeur → BLE, sur une seule aide, en continu.

- Remplacer les trames factices de la Phase 2 par le vrai flux encodé de la Phase 4.
- Tenir la cadence 20 ms en continu (plusieurs minutes) sans dérive ni sous/sur-alimentation du
  buffer de trames.
- Écoute réelle côté aide : qualité, latence perçue, absence de coupures.

**Critère de sortie** : audio TV entendu clairement et en continu sur une aide, qualité jugée
acceptable par l'utilisateur.

## Phase 6 — Duplication sur les deux oreillettes + orchestration

**Objectif** : le système complet tel que décrit dans l'architecture.

- Étendre à la deuxième connexion/canal CoC, envoi synchronisé (même tick 20 ms).
- State machine complète : détection de signal TV → START automatique ; silence prolongé → STOP
  automatique ; gestion de la perte de connexion BLE avec reconnexion et reprise du stream.
- Test d'usage réel prolongé (heures) devant la TV.

**Critère de sortie** : usage quotidien fonctionnel sans intervention manuelle — allumer la TV
suffit à démarrer le relai, l'éteindre l'arrête, une coupure BLE ponctuelle se rétablit seule.

## Phase 7 — Finition (optionnelle, selon retour d'usage)

- Réglage de volume physique (potentiomètre → mappé sur la characteristic Volume).
- Boîtier, alimentation définitive, câblage propre côté prise TV.
- Documentation utilisateur courte si le boîtier doit être manipulé par quelqu'un d'autre que
  l'utilisateur développeur.

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
