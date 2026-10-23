# Diagnostic USB natif ESP32-C6 — Bazzite / Podman

Cette variante conserve IDF **5.5.4**, Zigbee **2.0.5**, le rôle SED avec RX-off,
les endpoints **10..13** et le polling **1000 ms**. Elle utilise
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` sur le port USB-C natif et passe
`esp_pm_configure().light_sleep_enable` à **false**. PM et tickless idle restent
compilés, mais l'application n'autorise plus l'entrée automatique en light-sleep.
La radio Zigbee peut rester inactive entre les polls ; ce mode ne transforme pas
l'appareil en routeur ni en récepteur permanent.

La version normale garde la console UART et le light-sleep autorisé. L'option
`OPF_ZB_USB_DIAGNOSTIC` vaut n par défaut et dépend de la console USB. L'overlay USB
et ses fichiers sdkconfig/build sont distincts : ne pas réutiliser le sdkconfig USB
pour les essais sur piles. Aucun changement CC1101, Profalux ou convertisseur Z2M.

## Compilation reproductible

Depuis la racine du dépôt sur Bazzite :

```bash
podman run --rm --network=host -v "$PWD":/project:Z \
  -w /project/firmware-zigbee docker.io/espressif/idf:v5.5.4 \
  bash tools/build-usb-debug.sh
```

Le script compose `sdkconfig.defaults;sdkconfig.usb-debug.defaults`, fixe la cible
esp32c6 et utilise exclusivement `build-usb-debug/sdkconfig`. Les variantes
normales se compilent avec `bash tools/build-matrix.sh` dans le même conteneur,
avec leurs configurations séparées. Les scripts vérifient les options générées.
Si un ancien sdkconfig local a été modifié avec menuconfig, les defaults ne le
remplacent pas : retirer uniquement le dossier de build concerné pour repartir
sur les valeurs de référence (aucune action sur la NVS de la carte).

## Artefacts du build exécuté

Chemins depuis la racine du dépôt :

| Artefact | Chemin | Offset flash |
| --- | --- | --- |
| Bootloader | `firmware-zigbee/build-usb-debug/bootloader/bootloader.bin` | `0x0` |
| Table de partitions | `firmware-zigbee/build-usb-debug/partition_table/partition-table.bin` | `0x8000` |
| Application | `firmware-zigbee/build-usb-debug/profalux_zigbee_sed.bin` | `0x10000` |
| ELF pour décodage des logs | `firmware-zigbee/build-usb-debug/profalux_zigbee_sed.elf` | Pas à flasher |
| Arguments générés par IDF | `firmware-zigbee/build-usb-debug/flash_args` | Trois segments ci-dessus |
| Configuration finale | `firmware-zigbee/build-usb-debug/sdkconfig` | Pas à flasher |

Application : **523552 octets**, `0x7fd20`, 50 % libres dans la partition de 1 Mo.
Flash configurée à 4 Mo, DIO, 80 MHz. Les
[empreintes et tailles](validation/usb-debug-build-results.json) décrivent ce build.
Les binaires et configurations générés sont ignorés par Git et restent disponibles
localement ; un nouveau build, notamment après commit, peut changer leur hash.

Table décodée depuis le binaire réellement produit :

| Nom | Type / sous-type | Offset | Taille |
| --- | --- | --- | --- |
| nvs | data / nvs | `0x9000` | `0x6000` (24 Kio) |
| phy_init | data / phy | `0xf000` | `0x1000` (4 Kio) |
| factory | app / factory | `0x10000` | `0x100000` (1 Mio) |

Cette table est identique à celle des variantes normales compilées. Si la carte
possède un autre firmware/table de partitions, vérifier d'abord que sa partition
NVS occupe le même emplacement avant d'utiliser ces commandes : une table neuve
ne migre pas les anciennes données. Ne pas employer un binaire fusionné rempli
sur les trous de flash pour cet essai.

## Commandes pour un essai ultérieur — non exécutées

Le port natif apparaît normalement comme `/dev/ttyACM*`. Identifier le bon port
avec `ls -l /dev/serial/by-id/`, puis adapter `PORT`. Le conteneur reçoit un seul
périphérique ; `keep-groups` conserve les droits série de l'utilisateur et
`label=disable` évite les blocages SELinux sur ce périphérique. Si l'accès est
refusé, vérifier les droits du port et les groupes de l'utilisateur sur l'hôte.
Le montage de sources n'utilise alors pas de réétiquetage `:Z`.

Depuis la racine du dépôt, flash des trois segments générés, **sans erase-all et
sans segment NVS** :

```bash
PORT=/dev/ttyACM0
podman run --rm -it --userns=keep-id --group-add keep-groups \
  --security-opt label=disable --device "$PORT:/dev/ttyACM0" \
  -v "$PWD":/project \
  -w /project/firmware-zigbee/build-usb-debug \
  docker.io/espressif/idf:v5.5.4 \
  python -m esptool --chip esp32c6 --port /dev/ttyACM0 --baud 460800 \
  --before default_reset --after hard_reset write_flash @flash_args
```

Le chargeur efface les secteurs des trois segments écrits, pas la partition NVS.
La commande ne doit être lancée que pour une session d'essai autorisée : au
redémarrage, le firmware lance la pile et tente l'association ou la reprise réseau.
Pour cette étape, ni cette commande ni celle de monitoring n'ont été exécutées.

Après réénumération USB, vérifier de nouveau `PORT`, puis lancer le monitoring
sans reset automatique :

```bash
PORT=/dev/ttyACM0
podman run --rm -it --userns=keep-id --group-add keep-groups \
  --security-opt label=disable --device "$PORT:/dev/ttyACM0" \
  -v "$PWD":/project \
  -w /project/firmware-zigbee/build-usb-debug \
  docker.io/espressif/idf:v5.5.4 \
  python -m esp_idf_monitor --target esp32c6 --port /dev/ttyACM0 --baud 115200 \
  --no-reset profalux_zigbee_sed.elf
```

Quitter avec **Ctrl+]**. Le débit est le paramètre du moniteur ; l'USB CDC natif
ne constitue pas une liaison UART physique à 115200 bauds. En cas de changement de
numéro ACM après reset, relancer le conteneur avec le nouveau port. Si le bootloader
USB ne répond pas, utiliser la procédure BOOT/RESET propre à la carte pour entrer
en téléchargement, sans effacement de flash.

## Logs attendus, à valider sur carte

Le démarrage indique `BOOT USB diagnostic`, IDF/SDK, l'initialisation NVS, puis
`Initializing Zigbee`, `Zigbee initialized`, les endpoints et le lancement de la
mainloop. BDB indique factory-new ou réseau restauré ; le steering affiche succès
avec canal/adresse ou échec avec nouvelle tentative dans 30 s. Chaque commande
acceptée est consommée dans un log `SIMULATION endpoint=... command=OPEN|CLOSE|STOP
received_us=...; no RF transmission`. Les commandes refusées et la saturation ont
leurs propres logs. Un succès de réception ne confirme aucun déplacement de volet.

Les premiers messages peuvent précéder l'ouverture du moniteur ; le build ne
retarde pas le démarrage en attendant un terminal. Aucune séquence de logs sur
matériel, énumération USB ou association réelle n'est encore validée.
