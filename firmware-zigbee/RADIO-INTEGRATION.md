# Étape 4 — intégration logicielle CC1101 / Profalux

24 octobre 2026, branche `feature/zigbee-gateway-autonomous`, base `1e9a44d`.
**Changements à revoir avant commit : aucun commit, push ou PR créé.**

Contexte matériel communiqué par l'utilisateur : le firmware précédent a rejoint
Zigbee2MQTT 2.14.2 et reçu les douze commandes des endpoints 10..13 via USB natif.
Ces essais antérieurs ne constituent pas une validation matérielle du nouveau port
radio. Aucun CC1101 n'a été connecté, aucun firmware flashé, aucune émission ou
modification du réseau existant n'a été exécutée pendant cette étape.

## Architecture livrée

```text
callback Zigbee inchangé côté protocole
  -> file bornée (8 commandes, section critique courte)
  -> worker radio unique (hors verrou Zigbee)
       -> SIMULATION : log de réception, aucun GPIO/SPI/RMT radio
       -> RADIO_DIAGNOSTIC : sonde SPI, capture RX seulement sur activation locale
       -> RADIO_ACTIVE : réservé au futur, émission toujours verrouillée
       -> mapping endpoint / OPEN-CLOSE-STOP / captures / fréquence
       -> NVS du mapping dans un namespace distinct
```

Le modèle Zigbee, ses quatre endpoints, le convertisseur et les dépendances
ESP-IDF **5.5.4** / Zigbee **2.0.5** restent inchangés. Le callback n'effectue ni
SPI, ni NVS, ni attente radio. Il acquitte l'acceptation dans la file, pas une
exécution RF. Une file pleine retourne toujours `INSUFFICIENT_SPACE`. Le worker
journalise SIMULATION, NOT_READY, NO_DATA ou TX_LOCKED ; il ne publie pas de
succès de mouvement. Une erreur de sonde ne provoque pas d'abort : la tâche radio
continue et la pile Zigbee démarre indépendamment. Ce comportement est vérifié
logiquement avec des mocks, pas encore par un démarrage de cette version sur carte.

La file est réellement celle testée sur hôte (`pfx_queue_push/pop`) ; son accès
est sérialisé par une section critique ESP32. Les traitements sont bornés à huit
commandes par tour pour repasser par l'expiration et les contrôles locaux. La file
locale de contrôle comporte quatre entrées ; elle est indépendante de la file ZCL.

**Aucun chemin TX actif** : l'ancien `cc1101.c` n'est pas compilé dans le firmware
Zigbee ; sa table de registres est extraite à la configuration CMake. Le nouveau
port ne configure pas PATABLE et ne contient pas de séquence d'émission. Son API
`pfx_cc_transmit` retourne systématiquement TX_LOCKED. Le strobe STX `0x35` est en
plus refusé à deux niveaux du transport. Le mode 2 ne lève aucun de ces verrous.
Il n'existe pas d'option Kconfig autorisant TX à cette étape.

## Modules historiques étudiés et réutilisation

| Module | Fonction réelle et dépendances | Décision |
| --- | --- | --- |
| `cc1101.c` | SPI/GPIO, configuration OOK, RMT RX/TX, timers, FreeRTOS ; la primitive SPI ignore son code retour, l'initialisation SPI utilise `ESP_ERROR_CHECK`, les écritures relisent les registres jusqu'à six fois. Plusieurs fonctions TX et un auto-test TX sont disponibles. Pas d'API SPWD publique. | Nouveau port SPI contrôlé dans le projet Zigbee. Extraction des 27 entrées de `s_regs`, réutilisation de la politique de relecture, lectures ID avec le bit burst requis pour les registres de statut. Aucun changement au fichier historique. |
| `rx_capture.c/h` | Collecte bornée des fragments RMT avec copie des données avant réutilisation et validation terminale `is_last` ; aucune dépendance réseau. | Compilé directement depuis `firmware/main`, utilisé dans le callback RMT du nouveau port. |
| `keeloq_decode.c/h` | Démodulation des impulsions, déglitch, recherche d'en-tête et vote des mots HCS30x ; ne déchiffre pas les compteurs KeeLoq. Buffers statiques partagés. | Compilé directement et appelé par l'unique worker, jamais simultanément par plusieurs tâches. |
| `rx_confirm.c/h` | Confirmation par répétition des 64 premiers bits, VLOW/RPT ignorés, fenêtre adaptative jusqu'à 4 s. | Compilé directement pour l'apprentissage des captures complètes. |
| `radio.c` | Arbitrage RX/TX, tâche d'écoute, sweep/calibration et capture dédiée ; cette dernière accepte aussi une trame isolée après un délai de grâce. | Étudié, pas importé : le nouveau worker sérialise les opérations, sans écoute permanente ni repli sur trame isolée. La capture de cette étape exige deux mots complets concordants. |
| `shutters.c` | Stockage des commandes up/down/stop et rejeu brut ; génération virtuelle via `pfx_enrol_frame`, compteur et position temporelle ; dépend de MQTT, Wi-Fi, OTA, JSON, SPIFFS. | Pas importé. Reprise du découpage on-air serial bits 32..59 et bouton bits 60..63 pour vérifier les captures, sans calculer de position. |
| `pfx_enrol.c`, `pfx_keys.c`, `keeloq.c` | Identités virtuelles 0x067, pool de clés, construction de trames et compteurs 16 bits. Le chemin de commande réserve le compteur suivant puis émet la même valeur dans toute la rafale. Les helpers de sauvegarde historiques ne remontent pas tous leurs échecs à l'appelant. | Étudiés, pas d'enrôlement, de dérivation de clé ou de génération de compteur recopié. Une future activation TX exigera un service de compteur avec retour de sauvegarde contrôlé, anti-retour et politique d'épuisement. |
| `cfg_store.c`, `cfg_model.c` | Documents JSON répartis en NVS ; modèle couplé aux volets historiques et migration de dataset vers SPIFFS. | Non importés ; stockage réduit à une seule valeur NVS dédiée aux quatre mappings radio. |
| `radio_profile.c`, `hardware_config.h`, JSON SuperMini | Fréquence historique Profalux 868425 kHz ; JSON provisoire 868350 kHz. | Aucune fréquence choisie automatiquement en diagnostic SPI. La capture exige explicitement 868350 ou 868425 kHz. Aucun de ces profils n'est validé sur le module futur. |

La conservation du mot chiffré capturé n'apporte aucune garantie de rejeu accepté
par le moteur : le code roulant a pu avancer. Aucune capture n'est convertie en
compteur ou en clé. Les deux mécanismes historiques (rejeu brut et identité
virtuelle) restent distincts. Cette version prépare la capture et la correspondance
vers les commandes, sans prétendre résoudre le futur mode actif.

## Port SPI, présence et erreurs

Le port ESP-IDF utilise **SPI2, mode 0, 1 MHz**, CS manuel, sans DMA pour ces
transactions courtes. Les opérations SPI/GPIO remontent leur erreur ; la ligne
MISO/SO est attendue basse avant de cadencer, avec timeout de 10 ms. SRES attend
également la fin de CHIP_RDYn. Un module absent peut donc être signalé soit par
TIMEOUT, soit par ABSENT après une identité incohérente.

La sonde vérifie PARTNUM=0 et VERSION=0x04 ou 0x14. Une autre révision est refusée
jusqu'à validation explicite, même si elle pouvait être fonctionnelle. Après reset
et lecture, SIDLE puis SPWD sont envoyés ; aucune entrée en RX ou TX au démarrage.
SIDLE est vérifié avec MARCSTATE et un timeout borné. SPWD ne permet pas de lecture
de confirmation sans réveiller la puce : l'état logiciel SLEEP signifie que le
strobe a été accepté par le transport, pas qu'un courant de sommeil a été mesuré.

Ces séquences suivent la [fiche CC1101 de TI](https://www.ti.com/lit/ds/symlink/cc1101.pdf),
notamment l'attente de CHIP_RDYn et l'activation de SPWD lorsque CS remonte.
L'ancien profil OOK est relu après écriture avant RX ; les registres de fréquence
sont ensuite calculés pour le profil explicite avec un quartz supposé de 26 MHz.
Une erreur de lecture/écriture ou d'entrée en RX empêche l'apprentissage et provoque
une tentative de retour en sommeil. En cas d'erreur de transport, **l'état physique
ne peut pas être garanti** ; l'erreur est conservée et une nouvelle sonde explicite
est requise. Une ligne SPI inaccessible ne permet pas de promettre SPWD.

Les ressources SPI sont libérées sur échec d'initialisation. Le nettoyage RMT
n'abandonne pas ses buffers si le canal ne peut pas être supprimé : il journalise
une quarantaine, garde le verrou PM et interdit un nouvel apprentissage jusqu'à
nouveau nettoyage ou redémarrage. Cette situation n'a pas été injectée dans le
pilote ESP réel ; elle devra être vérifiée sur banc matériel.

## Brochage provisoire et conflits

| Signal CC1101 | GPIO C6 |
| --- | ---: |
| MOSI | 18 |
| MISO / SO | 20 |
| SCLK | 19 |
| CSN | 2 |
| GDO0 (RMT RX) | 3 |
| GDO2 | Non connecté |

La [fiche C6](https://documentation.espressif.com/esp32-c6_datasheet_en.html) place
l'USB natif sur GPIO12/13 et les straps sur GPIO4/5/8/9/15 : aucun conflit direct
avec la proposition. SPI0/1 et les connexions flash du FH4 ne sont pas utilisés par
ce port SPI2. Les fonctions alternatives SDIO de GPIO18/19/20 ne sont pas activées.
La radio Zigbee est native au SoC et ne réserve pas ce bus externe. Cela ne valide
ni les pads, ni la continuité, ni les composants ajoutés sur le PCB SuperMini :
contrôle physique obligatoire. Le C6 reste alimenté en 3,3 V régulés, pas directement
par les trois NiMH chargés.

## Configurations et activation locale future

Les scripts de build sont à exécuter dans l'image IDF 5.5.4 avec Podman, sans
périphérique série pour cette étape :

```sh
podman run --rm --network=host -v "$PWD":/project:z \
  -w /project/firmware-zigbee docker.io/espressif/idf:v5.5.4 \
  bash tools/build-radio-diagnostic.sh
```

- `tools/build-usb-debug.sh` : console USB et CPU éveillé, **SIMULATION** par défaut,
  aucune console de contrôle radio actif.
- `tools/build-radio-diagnostic.sh` : overlay USB + `sdkconfig.radio-diagnostic.defaults`,
  sonde SPI au boot (**RADIO_DIAGNOSTIC**) et console radio locale. Échec de sonde
  non fatal si le module est absent. Polling 1 s.
- `tools/build-matrix.sh` : SIMULATION, console UART, light-sleep autorisé, pollings
  1/2/5 s et configurations séparées. Le mode SPI diagnostic peut être sélectionné
  ultérieurement dans un build normal par Kconfig, mais n'a pas été mesuré sur piles.

La console n'est disponible que dans le build radio-diagnostic USB, qui ne sert
pas à mesurer l'autonomie. Lors d'une future session matérielle autorisée, via le
monitor USB (procédure Bazzite dans `USB-DIAGNOSTIC.md`, remplacer le dossier par
`build-radio-diagnostic/`), les commandes locales sont :

```text
status
mode 0
mode 1
learn 10 OPEN 15 868350
cancel
clear 10
mode 2
```

`mode 0` retourne en simulation, `mode 1` refait la sonde SPI et demande le sommeil.
`learn` exige endpoint 10..13, OPEN/CLOSE/STOP, 1..60 secondes et une des deux
fréquences explicites. Il ne s'active pas au boot, en simulation, si la sonde a
échoué ou si le stockage est invalide. Deux captures concordantes de 66 bits sont
requises ; aucune émission ni trame d'enrôlement ne répond à la télécommande.
Succès, expiration, `cancel` ou changement de mode arrêtent le RMT et demandent
SPWD. La session n'est pas persistée et ne reprend pas après reboot.

`clear 10` efface **uniquement le binding radio de cet endpoint**, via une nouvelle
version complète du blob ; jamais la partition NVS ou les données Zigbee. C'est
une opération explicitement demandée pour réaffecter un endpoint à une autre
identité. Pendant l'apprentissage, clear est refusé. `mode 2` reste TX_LOCKED ; il
ne devient pas actif grâce à la présence de données. Ces commandes sont documentées
mais n'ont été exécutées que via les APIs de tests hôtes, pas sur une carte.

## Cohérence du stockage et apprentissage

Un binding fixe une identité capturée (serial), une fréquence et jusqu'à trois
mots on-air pour OPEN/CLOSE/STOP. Les captures suivantes du même endpoint doivent
avoir le même serial et la même fréquence ; changer de télécommande exige un
`clear` explicite afin de ne pas mélanger ses commandes.

Le namespace **`pfx_zb_radio`**, clé **`bindings_v1`**, contient un blob de **848 octets** :
magic, schéma 1, encodage little-endian explicite, quatre bindings et CRC32. Le
padding C n'est pas persisté. Les longueurs, caractères, identité et fréquence sont
validés avant chargement ; un blob corrompu/incompatible désactive les écritures
jusqu'à diagnostic, sans migration ou effacement implicite. Un appareil neuf
conserve simplement quatre bindings vides.

Le worker construit une copie candidate, vérifie `nvs_set_blob` puis `nvs_commit`,
et ne publie la nouvelle copie en RAM qu'en cas de réussite. Un échec ambigu de
commit interdit toute nouvelle écriture avant reboot. Une coupure peut faire
perdre la nouvelle valeur en cours, mais pas produire un mélange de plusieurs clés
par endpoint. Ce choix repose sur la récupération de NVS décrite par
[Espressif pour IDF 5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32c6/api-reference/storage/nvs_flash.html#security-tampering-and-robustness).
Le test hôte vérifie l'ancien/nouveau snapshot et le rejet de données incohérentes ;
**il ne remplace pas un test de coupure sur la flash réelle**, notamment avec une
NVS partagée et presque pleine. Aucune donnée de la NVS matérielle n'a été modifiée.

## Sommeil, concurrence et mémoire

Aucun canal RMT ni buffer RX n'est alloué au repos. L'apprentissage alloue deux
buffers de 1024 symboles (8 Kio au total), puis les libère. Le décodeur historique
réserve aussi ses tableaux statiques dans BSS. Les sessions ne sont pas une écoute
permanente : cela préserve le choix d'une future passerelle sur NiMH.

Le port prend un verrou `ESP_PM_NO_LIGHT_SLEEP` pour chaque transaction SPI, et le
maintient pendant la session RX jusqu'à la libération du RMT et le retour SPWD.
L'état au repos ne conserve pas de verrou applicatif radio. La pile Zigbee reste
séparée et n'est pas bloquée par l'attente de capture ; le worker vérifie le délai
d'expiration toutes les 50 ms environ pendant la session, plus l'ordonnancement et
les opérations en cours. L'USB diagnostic conserve le CPU éveillé comme auparavant.

Les mesures RAM/flash des cinq compilations sont dans
`validation/step4-build-results.json`. La RAM de liaison inclut données/BSS et code
résident ; **elle ne mesure pas le heap libre au démarrage**. Le worker radio
réserve 6144 octets de stack via FreeRTOS (au lieu des 2048 du worker de simulation
précédent) ; la console locale ajoute 3072 octets dans son seul build. Ajouter TCB,
files et allocations SPI/RMT au bilan dynamique. Les mesures de high-water marks,
heap minimum, latence STOP et courant devront être faites sur carte. Aucune
estimation d'autonomie n'est produite.

## Fichiers et vérifications

- Ajout du composant `components/pfx_radio/` : API portable, port CC1101 contrôlé,
  mapping/file/apprentissage et adaptateur ESP-IDF (SPI/RMT/NVS/worker/console).
- Ajout `test/radio/` et `tools/extract-radio-config.py` : tests hôtes et profil
  historique généré sans recopier le pilote complet.
- Ajout overlay, script de build radio-diagnostic, collecte RAM/flash et ce rapport.
- Modification de `main/main.c`, son CMake et Kconfig pour relier la file radio.
  Les callbacks Zigbee et le modèle ZCL conservent leur sémantique de commande.
- Documentation et résultats actualisés dans le seul projet `firmware-zigbee/`.
  `firmware/`, la branche `feature/esp32c6-support`, les fichiers de clés, le lock
  des dépendances et le convertisseur Z2M n'ont pas été modifiés.

Les résultats détaillés des builds, tests et difficultés sont consignés dans
`validation/step4-checks.md`. Les anciens rapports d'étapes 3/USB restent historiques ;
leurs tailles et empreintes ne décrivent pas les binaires de cette intégration.

## Validation matérielle à réaliser après revue

1. Vérifier schéma/révision de SuperMini, continuité des six liaisons, GDO0 sans
   composant concurrent, GDO2 libre, tension régulée 3,3 V et masse commune.
2. Alimenter le CC1101 sans émission autorisée et lancer le build radio-diagnostic.
   Vérifier SPI mode 0 / 1 MHz au logique : CS, attente MISO, SRES, PARTNUM et VERSION,
   puis SIDLE/SPWD. Contrôler absence de STX avec capture SPI, sans auto-test TX.
3. Déconnecter le module et tester MISO haut/bas, défaut de câblage ou perte
   d'alimentation. Le firmware doit continuer Zigbee et afficher l'erreur sans
   reset ni faux succès. Ne pas court-circuiter les sorties pour provoquer ces tests.
4. Vérifier la même identité Zigbee et les douze commandes Z2M, sans nouvel
   appairage. Comparer les latences au firmware matériel précédent, y compris
   les rafales et la saturation ; aucune trame 868 MHz ne doit sortir du C6/CC1101.
5. Après validation de RX, activer une courte session `learn` sur le bon profil,
   actionner la télécommande d'origine, vérifier confirmation/mapping, annulation,
   expiration et SPWD. Contrôler au sniffer la réception seule, puis tester les
   quatre endpoints. Un mauvais bouton appris reste une erreur utilisateur à détecter.
6. Couper l'alimentation pendant une sauvegarde sur banc : au reboot accepter un
   snapshot valide ancien ou nouveau, jamais un mapping partiel/incohérent.
   Vérifier aussi le refus d'écriture quand la NVS est pleine, sans effacer le réseau.
7. Mesurer sommeil du CC1101, libération des verrous PM, courant pack et latence
   pour 1/2/5 s, heap minimum et stack restante. L'USB doit être débranché pour
   caractériser les variantes basse consommation.
8. Une étape distincte décidera rejeu ou identité virtuelle, migration des clés et
   compteurs, réservation durable, doublons/expiration/STOP prioritaire, puissance,
   fréquence et séquences TX. Aucun de ces verrous ne doit être levé par un simple
   changement de mode de la version présente.
