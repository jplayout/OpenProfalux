# Diagnostic USB — résultats du 23 octobre 2026

Branche : `feature/zigbee-gateway-autonomous`. Tests et compilations uniquement,
sans port série, flash, effacement NVS ou changement réseau.

## Compilation

Podman, `docker.io/espressif/idf:v5.5.4`, esp-zigbee-lib **2.0.5** depuis le lock
inchangé, cible ESP32-C6, compilateur RISC-V GCC 14.2.0.

| Build | Résultat | Application |
| --- | --- | ---: |
| USB diagnostic, 1000 ms | PASS | 523552 octets, `0x7fd20` |
| Normal UART/light-sleep, 1000 ms | PASS | 535040 octets, `0x82a00` |
| Normal UART/light-sleep, 2000 ms | PASS | 535040 octets, `0x82a00` |
| Normal UART/light-sleep, 5000 ms | PASS | 535040 octets, `0x82a00` |

Commandes utilisées dans le conteneur, depuis `firmware-zigbee/` :
`bash tools/build-usb-debug.sh` et `bash tools/build-matrix.sh`.
Les logs complets sont dans `build-usb-debug.log`, `build-normal-regression.log`
et `build-validation/build-{1000,2000,5000}.log`. Aucun warning/error relevé dans
les logs des compilations finales. Les empreintes figurent dans
`usb-debug-build-results.json` et `build-results.json`.

Contrôles sur sdkconfig : USB Serial/JTAG actif, diagnostic actif, ZED et 1000 ms
pour le diagnostic ; UART, PM, tickless idle et diagnostic absent dans les trois
configurations normales. Dans le code compilé, l'option diagnostic sélectionne
`light_sleep_enable=false`, la branche normale conserve `true`. Ces contrôles ne
mesurent pas l'état électrique du C6 ni le fonctionnement de son USB sur carte.

La table de partitions a été décodée avec `gen_esp32part.py` : NVS 0x9000/24 Kio,
PHY 0xf000/4 Kio, application 0x10000/1 Mio. Les tables USB et normale sont identiques
octet pour octet. Les options `--no-reset`, `--target` et les commandes esptool ont
été vérifiées avec l'aide des outils de l'image IDF. Flash et monitoring réels
restent non exécutés ; la procédure Bazzite est dans `../USB-DIAGNOSTIC.md`.

## Tests existants exécutés

- **Zigbee2MQTT : PASS**, Node 22.23.3, zigbee-herdsman-converters 26.123.0,
  `node profalux-gateway.test.mjs` dans le conteneur Node hors réseau. Les quatre
  endpoints, 12 commandes, exposes SET-only, erreurs de livraison et absence
  d'état optimiste sont contrôlés.
- **Capture RMT : PASS**, les 17 assertions du banc existant, compilé via CMake
  avec AddressSanitizer dans l'image IDF 5.5.4 (compilateur hôte GCC 13.3).
  Commandes : `cmake -S /project/firmware/test/rx-capture -B /tmp/rx-tests`,
  `cmake --build /tmp/rx-tests`, `/tmp/rx-tests/rx_capture_host_test`.
- **KeeLoq historique : 11 réussites, 2 échecs**, code retour 1. Même résultat
  reproduit en extrayant avec `git show HEAD:<fichier>` les sources du commit de
  départ dans `/tmp/opf-usb-baseline/`, puis en recompilant indépendamment.
  Échecs préexistants `normal_learning(serial=0x1ee2b949)` et
  `secure_learning(0x11..., seed=0x22...)`. Ces fonctions et leurs tests sont
  inchangés par la variante USB. Ce résultat n'est pas présenté comme une réussite.

Commande KeeLoq sur les sources courantes, depuis la racine :

```sh
cc -Wall -Wextra -O2 -DSIM_MODE -Ifirmware/main -Isim \
  sim/test_keeloq.c firmware/main/keeloq.c firmware/main/pfx_keys.c \
  -o /tmp/opf-usb-keeloq-test
/tmp/opf-usb-keeloq-test
```

Le premier essai du banc RMT directement sur Bazzite a échoué à l'édition de liens
car `/usr/lib64/libasan.so.8.0.0` est absente. Le test a ensuite réellement réussi
dans Podman avec ASan, sans désactiver la vérification mémoire. Une première
extraction du banc KeeLoq de référence référençait par erreur un fichier sdkconfig
inexistant ; l'extraction corrigée a permis de reproduire les deux échecs ci-dessus.

`bash -n` sur les deux scripts de build et `git diff --check` : PASS.
Aucune fonction CC1101 ou de protocole Profalux n'a été modifiée. Les autres bancs
applicatifs du firmware Wi-Fi/MQTT ne font pas partie de cette validation Zigbee.
