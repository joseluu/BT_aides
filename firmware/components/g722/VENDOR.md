# Encodeur G.722 (copie de sippy/libg722)

- Source : https://github.com/sippy/libg722, commit 4c2e79c5cbcb5ee12a97a16002a073ac83396480
- Fichiers repris tels quels : g722_encode.c, g722.h, g722_common.h, g722_encoder.h,
  g722_private.h, LICENSE (decodeur non repris).
- Licence : domaine public (Steve Underwood) + code CMU 1993 a usage libre (voir LICENSE).
- Meme lignee SpanDSP que l'encodeur G.722 de la pile Bluetooth d'Android
  (AOSP packages/modules/Bluetooth/system/embdrv/g722), utilise pour l'ASHA.
- Verifie bit-exact avec l'encodeur g722 de ffmpeg sur 10 s de musique (80 920 octets).
