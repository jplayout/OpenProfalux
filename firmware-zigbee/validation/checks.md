# Vérifications du 23 octobre 2026

## Firmware Zigbee

Podman, image locale `docker.io/espressif/idf:v5.5.4` (ID `209569617d40`).
`idf.py --version` / résolution composant : ESP-IDF **5.5.4**, Zigbee **2.0.5**.
Compilateur RISC-V GCC **14.2.0**. Commande exécutée depuis la racine :

```sh
podman run --rm --network=host -v "$PWD":/project:Z \
  -w /project/firmware-zigbee docker.io/espressif/idf:v5.5.4 \
  bash tools/build-matrix.sh
```

| Intervalle | Résultat | Taille application |
| --- | --- | ---: |
| 1000 ms | PASS | 533840 octets (`0x82550`) |
| 2000 ms | PASS | 533840 octets (`0x82550`) |
| 5000 ms | PASS | 533840 octets (`0x82550`) |

Les configurations produites sélectionnent ZB_ZED, PM, tickless idle et la valeur
de polling attendue. La partition application de 1 Mo garde 49 % libres. Flash
physique configurée à 4 Mo. Les empreintes et tailles du dernier rebuild (avec diagnostic USB) sont désormais
dans `build-results.json` ; voir aussi `usb-debug-checks.md`. Les tailles de cette
section décrivent la compilation initiale de l’étape 3.
Aucun warning/error dans les trois logs de cette matrice finale.

La première compilation de l'ébauche avait échoué sur une constante inutilisée
(`s_window_covering_commands`) ; elle a été supprimée. Un build intermédiaire puis
la matrice finale ont réussi. Aucune compilation Zigbee sous 6.1 n'a été forcée.
L'image 6.1 a seulement été inspectée (`idf.py --version`, présence du composant
legacy driver et du header RMT) ; cette inspection ne démontre pas sa compatibilité
avec les bibliothèques Zigbee précompilées.

## Banc CC1101 isolé sous 5.5.4

Projet temporaire `/tmp/opf-zigbee-research/radio-check`, sources du dépôt montées
en lecture seule, `app_main` vide. Build `idf.py -DIDF_TARGET=esp32c6 build` :
**PASS**, ELF et image générés. Sources réellement compilées sans adaptation :

- `cc1101.c`, `rx_capture.c`, `keeloq_decode.c`, `radio_profile.c`, `keeloq.c`.
- `board_pins.h` généré avec `tools/gen_board.py` depuis le JSON SuperMini existant.
- Dépendances : `esp_driver_spi`, `esp_driver_gpio`, `esp_driver_rmt`, `esp_timer`.

Ce banc vérifie la compilation des unités, notamment les API de RX partiel RMT.
Comme aucune fonction radio n'est appelée, le linker peut éliminer leur code ; il
ne prouve pas l'intégration finale ni le fonctionnement RF. Il n'inclut pas
`shutters.c`, `pfx_enrol.c` ou la pile Zigbee. La configuration de flash par défaut
de ce banc temporaire n'est pas celle du livrable FH4 ; ce binaire ne doit pas être
utilisé sur matériel. Aucun warning/error relevé dans son build final.
Une première tentative a échoué sur l'accès au montage SELinux ; l'option `:ro,z`
a permis la compilation effective.

## Convertisseur hors ligne

Node **22.23.3** dans Podman, paquet réel **zigbee-herdsman-converters 26.123.0**
installé dans `/tmp/opf-zigbee-research` ; aucun service Z2M contacté.
`node profalux-gateway.test.mjs` : **PASS**.

Contrôles : préparation de la définition par le paquet, mapping des quatre endpoints,
mode multiEndpoint, 12 commandes, accès SET seul, absence de position et de réponse
optimiste, rejet d'une valeur invalide/endpoint absent, propagation d'erreur de livraison.
Le destinataire herdsman est simulé : pas d'interview, d'ACK réel ou de latence mesurée.

## Conservation et limites

Empreintes de tous les fichiers modifiés/non suivis préexistants hors prototype
comparées à la capture initiale : **inchangées**. Référence `feature/esp32c6-support`
conservée à `545b3bd`. Les modifications hors prototype restent dans le répertoire
de travail et ne sont pas incluses dans le travail Zigbee.

Non exécutés : flash, effacement NVS, émission RF, association Zigbee, changement
Z2M, mesures électriques, tests de reprise réseau sur carte. Les résultats des
anciens tests ESP32-C6 restent conservés ; ils n'ont pas été présentés comme des
validations du nouveau firmware Zigbee.
