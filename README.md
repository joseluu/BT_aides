# BT_aides

Emetteur audio pour aides auditives Bluetooth LE (protocole ASHA, Android Hearing Aid Profile) :
un ESP32-S3 numerise un signal stereo (PCM1808, I2S), l'encode en G.722 16 kHz et le diffuse
vers deux aides (gauche et droite) en L2CAP CoC, avec NimBLE (ESP-IDF, PlatformIO).

- `firmware/` : application principale (capture, encodage, orchestration des deux aides,
  interface a encodeur rotatif et LED).
- `test_ui/` : test materiel de l'encodeur et de la LED.
- `docs/architecture.md` : architecture et choix techniques.
- `docs/plan_implementation.md` : journal des phases, mesures et decisions.

Fonctionnellement equivalent au Signia TV Sound (transmetteur audio pour aides Signia), mais
ouvert et base sur le profil ASHA. Teste avec des aides auditives Signia 7IX. Projet independant,
non affilie a Signia.

Compilation : `cd firmware && pio run -e xiao_esp32s3` (ESP32-S3, framework ESP-IDF).
Le projet est un travail de recherche : la paire d'aides ciblee est choisie par son HiSyncId
(`g_hisyncid_by_pair` dans `firmware/main/main.c`).

Licence : MIT.
