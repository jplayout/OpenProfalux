# Étape 4 — résultats logiciels du 24 octobre 2026

Base `1e9a44d`, branche `feature/zigbee-gateway-autonomous`. Aucun commit ou push
créé pour cette étape. Les essais Zigbee physiques communiqués par l'utilisateur
concernent la version précédente ; aucune nouvelle validation sur carte ici.

## Compilations exécutées

Podman local, image `docker.io/espressif/idf:v5.5.4`, GCC RISC-V 14.2.0,
espressif/esp-zigbee-lib 2.0.5 (lock et manifeste inchangés).

| Variante | Polling | Console / mode au boot | Application (octets) | RAM de liaison (octets) | Reste RAM selon map |
| --- | --- | --- | ---: | ---: | ---: |
| USB simulation | 1 s | USB, SIMULATION | 575488 | 121838 | 330274 |
| USB diagnostic SPI | 1 s | USB + contrôles locaux, RADIO_DIAGNOSTIC | 576864 | 121838 | 330274 |
| Normale | 1 s | UART, SIMULATION, light-sleep autorisé | 586032 | 123022 | 329090 |
| Normale | 2 s | UART, SIMULATION, light-sleep autorisé | 586032 | 123022 | 329090 |
| Normale | 5 s | UART, SIMULATION, light-sleep autorisé | 586032 | 123022 | 329090 |

**PASS sur les cinq builds**, sans warning/error du compilateur dans les logs
finaux. Les chemins et SHA-256 figurent dans `step4-build-results.json`. Les
configurations finales ont été vérifiées (ZED, polling, mode radio, activation de
la console USB). Les cinq tables de partitions sont identiques à celle du build
USB : NVS 0x9000/24 Kio, PHY 0xf000/4 Kio, factory 0x10000/1 Mio.

Ces chiffres de RAM incluent le code placé en mémoire interne : USB = 12352 octets
de data + 19488 de BSS + 89998 de code ; normale = 12544 + 19488 + 90990.
Ils ne comprennent pas les futures allocations du scheduler/driver et ne sont pas
une mesure de heap libre. L'apprentissage alloue 8192 octets de buffers RX ; le
worker utilise une stack de 6144 octets, la console optionnelle 3072 octets, plus
les surcoûts FreeRTOS. Aucun courant ou temps de réponse sur carte n'a été mesuré.

Les maps des trois pollings sont conservées séparément dans
`build-validation/map-{1000,2000,5000}.map`. Les binaires normaux sont dans
`build-validation/profalux-{1000,2000,5000}.bin`, l'application USB dans
`build-usb-debug/profalux_zigbee_sed.bin` et le diagnostic SPI dans
`build-radio-diagnostic/profalux_zigbee_sed.bin`. Bootloader, table et ELF restent
dans chaque dossier de build. Aucun binaire fusionné ni commande de flash exécutée.

Pour reproduire les builds, dans le conteneur IDF depuis `firmware-zigbee/` :

```sh
bash tools/build-radio-diagnostic.sh
bash tools/build-usb-debug.sh
bash tools/build-matrix.sh
python tools/collect-radio-build-results.py
```

Les logs complets locaux sont dans `build-radio-diagnostic.log`,
`build-usb-debug.log`, `build-normal-regression.log` et
`build-validation/build-{1000,2000,5000}.log` (ignorés par Git).

## Tests hôtes exécutés

| Banc | Résultat | Portée |
| --- | --- | --- |
| Nouveau `firmware-zigbee/test/radio` | PASS | ASan + UBSan, transport SPI mocké, vraie file et vrai service de mapping/apprentissage. |
| Historique `firmware/test/rx-capture` | PASS | 17 assertions, collecte des fragments RMT, ASan. |
| Historique `firmware/test/rx-confirm` | PASS | 24 assertions, répétitions et confirmation adaptative, ASan. |
| Historique `firmware/test/keeloq` | PASS | 15 assertions, décodage d'impulsions OOK, ASan. |
| `z2m/profalux-gateway.test.mjs` | PASS | Node 22.23.3, zigbee-herdsman-converters 26.123.0, douze commandes et définition Z2M inchangée. |
| Historique `sim/test_keeloq.c` | **11 réussites, 2 échecs connus** | Même normal_learning et secure_learning que l'étape précédente ; algorithme/clés et test inchangés. |

La nouvelle suite couvre les absences MISO haut/bas et version inconnue, erreur
SPI init/lecture, timeout de disponibilité, relecture des écritures, refus de RX
non configuré, timeout RX, sommeil/réveil, verrou TX jusque dans RADIO_ACTIVE,
simulation sans puce, données absentes, douze bindings, invalidité des commandes,
FIFO et commandes rapprochées, saturation, bouclage, captures confirmées, TTL,
CRC/schéma/taille/contenu invalides, échec de sauvegarde sans mutation RAM et clear
persistant explicite. Tous les appels au faux bus vérifient l'absence de STX.

Les tests ne font tourner ni un bus SPI physique, ni le callback ESP RMT réel,
ni le backend flash NVS ; le modèle de coupure teste les snapshots, pas les
secteurs internes NVS. Le démarrage avec module absent reste un résultat logique
du chemin d'erreur non fatal, à confirmer sur l'ESP32-C6.

Reproduction des nouveaux tests : voir `../test/radio/README.md`. Les tests C sont
exécutés dans l'image IDF avec son compilateur hôte GCC 13.3 et ASan ; aucune
dépendance à la bibliothèque ASan manquante de Bazzite. Les tests Z2M ont été
exécutés dans le conteneur Node, avec le paquet installé dans
`/tmp/opf-zb-step4-z2m/` ; aucun broker ou coordinateur n'a été contacté.

Pour les trois bancs historiques, dans le conteneur :

```sh
cmake -S /project/firmware/test/rx-capture -B /tmp/rx-capture
cmake --build /tmp/rx-capture
/tmp/rx-capture/rx_capture_host_test
cmake -S /project/firmware/test/rx-confirm -B /tmp/rx-confirm
cmake --build /tmp/rx-confirm
/tmp/rx-confirm/rx_confirm_host_test
cmake -S /project/firmware/test/keeloq -B /tmp/keeloq
cmake --build /tmp/keeloq
/tmp/keeloq/keeloq_host_test
```

Le test d'algorithme historique a été compilé depuis la racine avec :

```sh
cc -Wall -Wextra -O2 -DSIM_MODE -Ifirmware/main -Isim \
  sim/test_keeloq.c firmware/main/keeloq.c firmware/main/pfx_keys.c \
  -o /tmp/opf-step4-keeloq-test
/tmp/opf-step4-keeloq-test
```

Échecs retrouvés : normal_learning obtenu `E8780163BB1E9542`, attendu
`0D107D43929D4778` ; secure_learning obtenu `80DFD3DF060A66BF`, attendu
`F2D8C1B388888888`. Ils ne sont pas masqués et aucune correction de protocole n'a
été introduite. Les suites du produit historique Wi-Fi/HTTP/OTA ne sont pas
présentées comme exécutées par cette étape.

## Difficultés, conservation et limites

L'extracteur a d'abord refusé le profil car sa garde de comptage attendait 28
entrées ; la table existante en comporte 27. La garde a été corrigée et le profil
extrait est utilisé par les tests et les builds. Le répertoire temporaire Node de
l'étape précédente n'existait plus : les dépendances ont été réinstallées dans un
nouveau dossier temporaire, puis le test a effectivement réussi.

Aucun nouvel échec final de test ou de compilation. Les retours de SPI, stockage
et RMT sont traités ; un échec de nettoyage RMT peut néanmoins maintenir le verrou
PM et interdire la reprise jusqu'au reboot, avec log explicite. La compatibilité
des versions CC1101 non 0x04/0x14 n'est pas présumée. La fréquence et le PCB restent
à valider physiquement, ainsi que les interruptions RMT, le sommeil et les coupures.

Les modifications restent dans `firmware-zigbee/`. `firmware/`, `sim/`, `boards/`,
les outils historiques, la branche ESP32-C6, le convertisseur et le lock Zigbee
n'ont pas été modifiés. Les anciens fichiers de résultats USB décrivent leur build
historique ; `step4-build-results.json` est la référence des binaires actuels.
Aucune action flash/NVS matériel, aucun STX physique, aucune modification réseau,
aucun commit/push/PR pendant cette étape.
