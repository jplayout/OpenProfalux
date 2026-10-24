# Correction de l'entrée USB native — ESP32-C6

La correction reste non committée sur `feature/zigbee-gateway-autonomous` pour revue.
Aucun port matériel ouvert, aucun flash ou changement NVS/réseau exécuté.

## Analyse du chemin effectif

Les sources de l'image `docker.io/espressif/idf:v5.5.4` ont été examinées :
`esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c`, `usb_serial_jtag.c`, leurs
en-têtes et les tests officiels `test_apps/usb_serial_jtag`.

`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` sélectionne le backend de console USB natif
utilisé par les flux standard. Cela n'installe pas à lui seul le pilote buffered
interrupt-driven. Le VFS utilise par défaut ses fonctions « basic » : lecture
non bloquante, écriture par attente sur la FIFO. Dans le VFS 5.5.4,
`usb_serial_jtag_vfs_use_driver()` remplace ces fonctions par le pilote ; une lecture
bloquante appelle alors `usb_serial_jtag_read_bytes(..., portMAX_DELAY)`.

L'application utilisait `getchar()` sans installer ce pilote ni sélectionner ce
mode VFS. Chaque EOF était effacé avec `clearerr(stdin)`, suivi d'une attente de
20 ms. Effacer EOF permet seulement une nouvelle lecture ; cela ne configure ni
le backend RX ni les interruptions USB. Le buffering libc et le mode du descripteur
n'étaient pas explicités avant le premier accès.

Le build examiné active les assertions, donc le `xTaskCreate()` placé dans
`configASSERT` devait y être évalué. Ce n'est pas une preuve que la tâche fonctionnait
sur la carte et cela ne suffit pas à diagnostiquer le timeout observé. La création
n'a désormais plus d'effet de bord caché dans cette assertion et son résultat est
journalisé. **La cause physique du timeout et sa disparition restent à confirmer
sur matériel** : le patch rend explicite et testable le chemin RX recommandé par
les API IDF, sans prétendre que le polling était à lui seul une preuve du défaut.

## Modifications

- `components/pfx_radio/usb_console.c` et son en-tête : initialisation par le seul
  lecteur, avant son premier `getchar()`. `stdin` passe en `_IONBF` ; `fileno(stdin)`
  et `fcntl` vérifient le descripteur et retirent `O_NONBLOCK`.
- Installation de `usb_serial_jtag_driver_install()` avec RX 1024 / TX 2048 octets,
  uniquement si aucun pilote n'est installé. Si présent, réutilisation et log.
  Les appels répétés après réussite ne refont pas l'initialisation.
- Sélection LF pour conserver les octets CR/LF traités par le parser existant,
  puis `usb_serial_jtag_vfs_use_driver()`. Pas de second enregistrement du VFS,
  pas de réouverture du port, de reset USB ou de désinstallation du pilote partagé
  avec stdout/stderr.
- `console_worker()` journalise son démarrage et l'état prêt ou l'erreur. Une
  initialisation refusée arrête seulement cette tâche, sans arrêt Zigbee/radio.
  Un seul handle de tâche est créé et une seule tâche lit stdin ; aucun REPL ou
  second appel à `usb_serial_jtag_read_bytes()` n'est ajouté.
- En fonctionnement connecté, `getchar()` attend une entrée dans le pilote plutôt
  que de sonder toutes les 20 ms. Un EOF/échec transitoire est signalé une fois,
  avec attente de 250 ms et `clearerr` pour permettre une reprise. Une commande
  partielle est abandonnée à cette erreur, pour ne pas fusionner deux sessions.
  Pas de heartbeat ou boucle active en attente de caractère.
- Le parsing de `status`, `mode`, `learn`, `cancel`, `clear`, les queues et le worker
  radio sont inchangés. Le CMake ajoute les dépendances USB/VFS explicites.
  L'initialiseur et la tâche restent conditionnés par `OPF_RADIO_CONSOLE` ; les
  configurations basse consommation et USB-simulation ne les activent pas.

Les logs de boot à rechercher sont : `USB console task created`,
`USB console worker started`, `USB Serial/JTAG driver installed` (ou `Reusing`),
puis `Console ready: native USB, interrupt-driven VFS, blocking unbuffered stdin fd=0`.
Le message ready prouve le passage dans la tâche, pas encore une réception réelle.
Chaque réponse à `status` prouvera ensuite que le chemin complet fonctionne.

## Vérifications logicielles

- Compilation radio-diagnostic USB sous IDF 5.5.4 / Zigbee 2.0.5 : **PASS**.
- Compilation USB-simulation : **PASS**, lecteur USB local désactivé.
- Compilation basse consommation séparée à 5 s : **PASS** ; aucun changement
  des defaults 1/2/5 s. Le symbole d’initialisation USB est absent de cet ELF
  et de celui de simulation USB, vérifié avec `nm`.
- Cinq tests de l'initialiseur avec ASan/UBSan : **PASS**. Ils couvrent installation,
  pilote déjà installé, erreur d'installation, erreur de buffering et de flags,
  ainsi que l'idempotence. Le VFS et les API USB sont mockés, pas le hardware.
- Suite radio portable existante avec ASan/UBSan : **PASS**.
- Script `tools/check-usb-status.py` : dix échanges simulés passent ; aucune commande
  n'a été envoyée au port réel. Le parsing des commandes du firmware n'a pas changé.
- Kconfig radio vérifié : USB_SERIAL_JTAG=y, OPF_RADIO_CONSOLE=y, OPF_RADIO_MODE=1,
  OPF_ZB_USB_DIAGNOSTIC=y, polling=1000, ZB_ZED=y, VFS_SUPPORT_IO=y.

Les tailles et SHA-256 actuels sont dans `validation/usb-console-fix-results.json`.
Le log de compilation est `build-usb-console-fix.log` (ignoré par Git).
Un premier build a révélé un include conditionnel placé avant le chargement de
`sdkconfig.h` ; l'ordre a été corrigé puis le build a réellement réussi.
Le conteneur IDF ne possède pas `rg` ; l'examen de ses sources a utilisé `grep`.
Les bancs algorithmiques KeeLoq, avec leurs deux échecs préexistants, n'ont pas été
relancés pour cette correction d'initialisation USB.

Reproduire les tests de l'initialiseur depuis la racine :

```bash
podman run --rm --network=none -v "$PWD":/project:ro,z \
  docker.io/espressif/idf:v5.5.4 bash -c \
  'cmake -S /project/firmware-zigbee/test/usb-console -B /tmp/usb-test && cmake --build /tmp/usb-test && ctest --test-dir /tmp/usb-test --output-on-failure'
```

## Reflash minimal à effectuer après revue — non exécuté

L'application corrigée est
`firmware-zigbee/build-radio-diagnostic/profalux_zigbee_sed.bin`.
Son ELF se trouve à côté. Pour la carte déjà flashée avec le layout de l'étape 4,
**seule l'application à 0x10000 doit être réécrite**. Bootloader et table de partitions
ne nécessitent aucune mise à jour ; le fichier généré `app-flash_args` contient seulement ce segment.
La NVS reste à 0x9000, taille 0x6000, entièrement avant l'application.

Depuis la racine sur Bazzite, vérifier le port (par exemple avec
`ls -l /dev/serial/by-id/`), fermer Monitor et tout autre lecteur, puis :

```bash
PORT=/dev/ttyACM0
podman run --rm -it --userns=keep-id --group-add keep-groups \
  --security-opt label=disable --device "$PORT:/dev/ttyACM0" \
  -v "$PWD":/project -w /project/firmware-zigbee/build-radio-diagnostic \
  docker.io/espressif/idf:v5.5.4 \
  python -m esptool --chip esp32c6 --port /dev/ttyACM0 --baud 460800 \
  --before default_reset --after hard_reset write_flash @app-flash_args
```

Ne pas employer `erase-flash`, le fichier `flash_args` complet ou un binaire fusionné
pour ce correctif. Si le layout de la carte diffère de celui déjà documenté,
le vérifier avant toute écriture ; cette procédure ne migre aucune partition.
Le reset final démarre le firmware et sa reprise Zigbee habituelle, sans nouvel
appairage demandé par le correctif.

## Test matériel répété de réception — non exécuté

Après réénumération USB, vérifier le port. Monitor doit être fermé pendant le test
PySerial, qui ouvre le port en exclusivité, avec timeout d'écriture de 2 s et délai
de réponse de 5 s. Le script n'envoie que `status\n`, jamais mode/learn/clear, et
ne demande pas d'impulsion BOOT/RESET. Dix réponses complètes sont requises :
une ligne STATUS suivie des bindings des endpoints 10 à 13 à chaque tour.

```bash
PORT=/dev/ttyACM0
podman run --rm --userns=keep-id --group-add keep-groups \
  --security-opt label=disable --device "$PORT:/dev/ttyACM0" \
  -v "$PWD":/project:ro -w /project \
  docker.io/espressif/idf:v5.5.4 \
  python firmware-zigbee/tools/check-usb-status.py --port /dev/ttyACM0 --repeat 10
```

Attendu : dix lignes `PASS status ...`, code retour 0, aucun timeout d'écriture.
Répéter après déconnexion/reconnexion USB. Puis arrêter le script et tester
manuellement `status` plusieurs fois dans Monitor, avec `--no-reset`, comme décrit
dans `USB-DIAGNOSTIC.md` en utilisant `build-radio-diagnostic/`.
Si le test échoue, conserver les logs de boot, le message exact du script et les
messages noyau USB ; ne pas conclure à un défaut du parser ou du CC1101 sans ces
éléments. Aucun résultat matériel positif n'est annoncé par les tests logiciels.
