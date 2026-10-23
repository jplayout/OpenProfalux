# Étape 3 — faisabilité de la passerelle autonome

Étude du 23 octobre 2026. Décision : prototype logiciel réalisable sur ESP32-C6FH4,
mais autonomie et latence de STOP non validées. La SuperMini est une carte de banc
à caractériser avant de retenir son alimentation sur accumulateurs.

## Périmètre et préservation

Le dépôt était déjà sur `feature/zigbee-gateway-autonomous` à l'ouverture de cette
étape, au même commit `545b3bd` que `feature/esp32c6-support`. Il contenait des
modifications ESP32-C6 non commitées et une ébauche `firmware-zigbee/`. Cette branche
expérimentale distincte a été conservée et complétée, sans déplacement de la
référence `feature/esp32c6-support`, ni modification des fichiers préexistants hors
`firmware-zigbee/`. Attention : des fichiers non commités appartiennent au répertoire
de travail, pas à une branche ; les changements de branche ne constituent pas leur
sauvegarde. Un diff initial et les empreintes des fichiers hors prototype ont été
sauvegardés dans `/tmp/openprofalux-before-zigbee*` pour contrôler leur conservation.

Aucun flash, accès série, effacement NVS, émission 868 MHz ou changement du réseau
Zigbee/Zigbee2MQTT n'a été exécuté. Le firmware compilé reste une simulation : ses
seules actions applicatives sont la réception et la journalisation des commandes.

## Versions retenues et limites de compatibilité

- **ESP-IDF 5.5.4**, image `docker.io/espressif/idf:v5.5.4`.
- **ESP-Zigbee-SDK / esp-zigbee-lib 2.0.5**, version exacte et hash dans
  `dependencies.lock`, cible `esp32c6`, bibliothèque **ZED native**, API v2 `ezb_*`.
- Flash configurée à **4 Mo**, conformément au suffixe FH4 ; le C6 intègre la radio
  802.15.4. Le boîtier FH4 possède moins de GPIO accessibles que le C6 QFN40 : le
  brochage CC1101 provisoire du dépôt exige une vérification sur la carte réelle.

Le [README officiel du SDK](https://github.com/espressif/esp-zigbee-sdk) recommande
5.5.4. Les [notes de version](https://raw.githubusercontent.com/espressif/esp-zigbee-sdk/main/RELEASE_NOTES.md)
annoncent 2.0.5 le 16 octobre 2026 sur cette base. Elles annoncent aussi le support
IDF **6.0** depuis 2.0.1 : il serait donc inexact de prétendre que toute version 6.x
est exclue. En revanche, ces sources ne confirment pas **6.1**. Le manifeste livré
avec 2.0.5 accepte simplement `idf >=5.0` : cette borne de résolution n'est pas une
matrice de certification des bibliothèques binaires. Le composant dépend encore
de `driver`, `vfs`, `ieee802154`, `mbedtls` et `openthread`.

Conclusion précise : **6.1 non confirmé par les sources consultées**, et non
« incompatibilité binaire démontrée ». Aucune combinaison 6.1/Zigbee n'a été forcée.
5.5.4 est retenu pour son statut de base recommandée et sa compilation effective.
Le manifeste applicatif limite volontairement l'essai à la série 5.5 à partir de
5.5.4 ; le lock résout exactement 5.5.4. Le produit OpenProfalux IDF 6.1 conserve
son projet et ses dépendances séparés.

## Firmware préparé

`main/main.c` initialise NVS sans chemin d'effacement automatique. Une erreur NVS
interrompt le démarrage plutôt que détruire l'identité réseau. Le SDK stocke son
réseau dans la partition `nvs` ; l'initialisation BDB restaure cette identité au
redémarrage. Seul un appareil factory-new lance le network steering. Les échecs de
steering/initialisation déclenchent une nouvelle tentative après 30 s, dans une
tâche distincte prenant le verrou Zigbee. Cette tâche reste bloquée en fonctionnement
normal. Une absence prolongée du parent peut néanmoins coûter beaucoup d'énergie.

Le rôle est End Device, `rx_on_when_idle=false`, timeout enfant demandé de 64 min,
PM et tickless idle activés, light-sleep automatique autorisé comme dans
[l'exemple officiel](https://github.com/espressif/esp-zigbee-sdk/tree/main/examples/sleepy_devices/light_sleep_end_device).
Il n'y a pas d'appel manuel à deep-sleep, qui ferait perdre le contexte RAM.
La console UART permet les mesures ultérieures sans USB natif connecté. L'entrée
effective en sommeil et les verrous PM devront être vérifiés sur carte.

Quatre endpoints **10 à 13**, profil HA, serveur Window Covering **0x0102**, reçoivent
`Up/Open 0x00`, `Down/Close 0x01`, `Stop 0x02`. Le callback vérifie commande et endpoint,
puis alimente une file de huit éléments sans bloquer. La tâche consommatrice inscrit
endpoint, commande et timestamp de réception dans le log `SIMULATION`. Une file pleine
renvoie `INSUFFICIENT_SPACE`. Une réponse ZCL positive signifie seulement que la
commande a été acceptée dans la file ; elle ne confirme aucun mouvement de volet.
Les autres commandes de mouvement sont refusées.

Les quatre attributs de position courante lift/tilt, absolue/pourcentage, sont
retirés du modèle produit par le helper SDK. Aucun timer n'estime une position ;
aucune commande ne met à jour une position. L'interview et les lectures directes
sur matériel devront confirmer leur absence et l'absence d'automatisme interne
indésirable du SDK. Les groupes/scènes ajoutés par le helper ne constituent pas une
fonction applicative validée ; utiliser des commandes unicast explicites par endpoint.

## Plusieurs volets et Zigbee2MQTT

La compilation valide la construction et l'enregistrement du modèle à quatre
endpoints ; sa découverte réelle reste à tester. Un seul enfant Zigbee héberge
plusieurs volets, avec un intervalle de polling commun et une file radio commune.
Les endpoints doivent rester stables après redémarrage. Une future table NVS
`endpoint -> volet/identité radio/profil` doit être versionnée et indépendante des
identifiants réseau Zigbee. Quatre est la taille du prototype, pas une limite
prouvée du SDK : toute augmentation demande de mesurer RAM, interview et saturation.

Pour ce produit nouveau, **prévoir le convertisseur externe fourni**, même si le
cluster est standard : Z2M doit reconnaître le modèle et exposer quatre commandes
sans slider ni état supposé. Une définition intégrée en amont pourra ensuite le
remplacer. Aucun convertisseur spécifique au protocole radio Profalux n'est requis
côté Z2M, puisque le C6 présentera le cluster standard.

`z2m/profalux-gateway.mjs` identifie fabricant et modèle, nomme `volet_1` à `volet_4`,
expose seulement `state` en accès SET avec OPEN/CLOSE/STOP, sans `fromZigbee`, position,
lecture d'état ou retour optimiste. Il demande une réponse ZCL. Les erreurs de
livraison restent des erreurs, jamais un état « ouvert ».

Selon la [documentation Z2M](https://www.zigbee2mqtt.io/advanced/more/external_converters.html),
les fichiers vont dans `external_converters/`, à côté de `configuration.yaml` ;
les installations neuves à partir de 2.11.0 désactivent par défaut les JS externes.
Vérifier `enable_external_js` selon la version effectivement installée lors de la
validation future. Rien n'a été installé ni changé sur notre instance existante.

## Réutilisation de CC1101 et Profalux sous IDF 5.5.4

Ne pas importer tout `firmware/main` : son manifeste et son CMake embarquent Wi-Fi,
MQTT, Ethernet, serveur web et OTA. `shutters.c` dépend directement de MQTT, Wi-Fi,
OTA, SPIFFS et JSON ; le réutiliser tel quel violerait l'architecture autonome.

Découpage proposé, sans modifier le portage 6.1 existant :

| Partie | Réutilisation / adaptation nécessaire |
| --- | --- |
| `cc1101.c`, `rx_capture.c`, `keeloq_decode.c`, `radio_profile.c`, `keeloq.c` | Composant radio/protocole sans transport ; dépendances explicites SPI, GPIO, RMT, timer. Préserver les corrections RX C6, buffers bornés et `SOC_RMT_MEM_WORDS_PER_CHANNEL`. |
| `hardware_config.h`, `gen_board.py` | Générer les broches depuis le JSON C6 ; vérifier routage, straps et SPI sur la carte physique. Aucun brochage n'est nécessaire dans la simulation. |
| `radio.c`, `rx_confirm.c`, `freq_sweep.c` | Réexaminer mutex et durée des sessions ; ne pas démarrer l'écoute continue pour une passerelle sur piles. Désactiver/libérer RMT hors transactions et placer CC1101 en sommeil. |
| Logique de commande dans `shutters.c`, `pfx_enrol.c` | Extraire encodage, clés, compteurs et stockage dans un service Profalux sans MQTT/Wi-Fi. Isoler les dépendances cJSON de la présentation et de l'enrôlement. |
| Adaptateur Zigbee | La file appelle à terme `submit(endpoint, OPEN/CLOSE/STOP)`. NVS/émission dans un worker, jamais dans le callback sous verrou de pile. |

Les API RMT modernes doivent être vérifiées sous **5.5.4** : RX partiel, flag
`is_last`, traitement ISR, allocation mémoire et GPIO après TX. Les fichiers
préexistants ne sont pas rétroportés globalement ; un banc de compilation séparé
contrôle leur compilabilité (résultat consigné ci-dessous). Les tests de capture
sur hôte et un essai RMT réel resteront nécessaires avant toute émission.

Prévoir un verrou PM pendant les transactions SPI/RMT, puis le relâcher ; la tâche
RF doit laisser tourner la pile Zigbee durant les rafales Profalux. Une file de
plusieurs volets impose une politique STOP prioritaire, expiration des anciennes
commandes et traitement des doublons APS avant activation RF. Une saturation ne
doit pas transformer un STOP en succès. Il faut aussi réserver les compteurs
rolling-code en NVS avant émission et valider la résistance aux coupures.

**Fréquence à résoudre avant intégration RF** : la demande cible 868,35 MHz, mais
`hardware_config.h` et `radio_profile.c` existants indiquent 868,425 MHz d'après des
captures antérieures, tandis que le JSON C6 indique 868350 kHz. Ne pas choisir
implicitement l'une de ces valeurs : définir le profil du volet à partir des
captures disponibles avant l'étape radio. Aucune fréquence n'est programmée ici.

## Trois AA NiMH et consommation

Trois éléments en série donnent 3,6 V nominaux ; leur capacité en Ah ne se multiplie
pas par trois. À titre de **dimensionnement hypothétique**, 1,0 à 1,45 V par élément
représentent 3,0 à 4,35 V, à remplacer par les courbes du modèle d'accumulateur
retenu, sous charge et en température. Le pack peut dépasser le maximum 3,6 V du C6 :
pas de connexion directe à 3V3. Le FH4 possède 4 Mo de flash intégrée au boîtier et
une alimentation recommandée 3,0–3,6 V selon la
[fiche Espressif](https://documentation.espressif.com/esp32-c6_datasheet_en.html).

La référence « SuperMini » ne fournit pas un schéma unique vérifié ici. Relever
fabricant, révision, marquage du régulateur, LED permanente/RGB, diviseurs résistifs,
USB-UART éventuel et chemin VBUS. Ne pas supposer un courant de repos à partir du
nom commercial. Ni une consommation de puce, ni les 7 µA de deep-sleep ne décrivent
cette carte en SED light-sleep. Espressif publie des valeurs typiques de light-sleep
180 ou 35 µA selon les domaines coupés ; leur reproduction dans ce firmware n'est
pas acquise.

Un LDO 3,3 V reçoit une marge décroissante quand le pack se décharge : sous
`3,3 V + dropout`, la sortie n'est plus régulée. Sa perte vaut approximativement
`(Vpack - 3,3) × Icharge + Vpack × Irepos`. Avec **4,2 V d'entrée hypothétiques**,
le rendement idéal hors courant de repos est 3,3/4,2 ≈ 79 %. À faible charge,
Irepos et LED peuvent dominer. Le pad 5V n'assure donc pas automatiquement une
alimentation correcte par trois NiMH. Étudier un buck-boost 3,3 V à faible Irepos,
compatible avec toute la plage du pack, son rendement à faible charge et ses
pointes ; vérifier aussi courants inverses et interactions avec USB.

Le [CC1101](https://www.ti.com/lit/ds/symlink/cc1101.pdf) annonce environ 200 nA en
sommeil et 14,7 mA en réception dans les conditions citées par TI, pas pour le
module assemblé. Une écoute 868 MHz permanente compromettrait le budget : conserver
la radio endormie hors rafales et éventuelles fenêtres RX bornées.

Modèle à remplir **après mesures** au point d'alimentation final :

`Ipack_moy(T) = Ibase_pack + Qpoll_pack / T + Σ(taux_événement × Qévénement_pack)`.

Les charges Q sont en mC et T en secondes pour obtenir des mA. Mesurer aussi les
retries, rejoin et association. Exemple purement illustratif, sans prédiction de
carte : `Ibase=0,25 mA`, `Qpoll=0,30 mC`, sans commande, donnent respectivement
0,55 / 0,40 / 0,31 mA pour 1 / 2 / 5 s. Une LED hypothétique de 1 mA ajouterait
24 mAh/jour, indépendamment du polling. Pour une mesure côté 3V3 plutôt que pack,
utiliser énergie/événement, rendement variable et Irepos sans les compter deux fois.
L'autonomie finale doit inclure capacité réellement utilisable avant dropout/coupure,
température, vieillissement et autodécharge NiMH. **Aucune autonomie n'est validée.**

## Polling, parent et STOP

| Poll nominal | Polls/heure | Attente idéale moyenne / maximum avant poll | Conséquence |
| --- | ---: | --- | --- |
| 1 s (défaut) | 3600 | 0,5 / 1 s | Meilleur candidat initial pour STOP, à mesurer. |
| 2 s | 1800 | 1 / 2 s | Compromis possible si délai utilisateur accepté. |
| 5 s | 720 | 2,5 / 5 s | STOP potentiellement trop tardif ; exige une marge de rétention du parent. |

Ces chiffres supposent commandes uniformément réparties, poll régulier, pas de
perte et file vide ; ce ne sont pas des bornes de bout en bout. Ajouter routage,
contention, retransmissions, file et future rafale 868 MHz. Le paramètre `keep_alive`
sert au cycle de keepalive/poll du SED ; vérifier la cadence effective au sniffer,
car le fast polling et les échanges réseau peuvent la modifier. Le délai enfant
de 64 min négocie le vieillissement de l'entrée enfant : il ne conserve pas une
commande pendant 64 min. Poll Control n'est pas implémenté dans ce prototype.

Un SED ne reçoit pas directement une commande lorsque sa radio dort : son **parent**
la garde jusqu'au poll, dans une file limitée et pour une durée limitée. Le parent
peut être un routeur autre que le coordinateur. Silicon Labs documente un
[timeout indirect de 7,68 s par défaut](https://docs.silabs.com/zigbee/9.0.0/zigbee-concepts-network/end-devices)
pour sa pile : ce n'est pas une garantie pour notre routeur/firmware. À 5 s, un seul
poll perdu peut déjà dépasser ce délai. La bonne condition est que l'attente avant
poll réussi, avec gigue et pertes, reste inférieure à la rétention effective et que
la file ne déborde pas. La file MQTT du serveur et les retries APS ne remplacent
pas cette condition. Des commandes broadcast/groupes ne sont pas un substitut
fiable à l'unicast vers cet enfant endormi.

## Plan de validation ultérieure avec notre installation

Aucun essai réseau ci-dessous n'a été réalisé à cette étape.

1. Relever sans changement les versions Z2M, zigbee-herdsman-converters, adaptateur,
   firmware du coordinateur, canal et routeurs disponibles. Identifier ensuite le
   parent réellement choisi et ses limites de buffer, rétention indirecte, enfants
   et vieillissement. Les références exactes de l'installation ne sont pas connues ici.
2. Sur une fenêtre d'essai convenue, sauvegarder la configuration Z2M, charger le
   convertisseur et associer la carte en simulation. Vérifier EndDevice et RX-off,
   quatre endpoints 10..13 et cluster 0x0102. Ne pas réinitialiser le réseau existant.
3. Envoyer individuellement `{"state_volet_1":"OPEN"}`, CLOSE puis STOP sur le topic
   `zigbee2mqtt/<nom>/set`, puis sur les trois autres endpoints. Vérifier les logs
   SIMULATION et les réponses ZCL. Tester commandes invalides, saturation et rafales
   OPEN/STOP/CLOSE. Aucun slider ni pourcentage ne doit apparaître, aucune position
   ne doit être déduite, y compris après relecture ou redémarrage.
4. Couper puis rétablir l'alimentation de la carte ; vérifier reprise de la même
   identité sans nouvel appairage ni effacement NVS, avec permit_join fermé. Tester
   parent temporairement absent puis revenu ; relever retries et délai de retour.
5. Pour chaque polling 1/2/5 s, effectuer au moins 300 commandes isolées à phases
   aléatoires et des rafales multi-endpoints. Relever p50/p95/p99/max, pertes,
   doublons et ordre, notamment STOP. Sniffer les Data Request, MAC ACK/Frame Pending,
   livraisons et APS/ZCL ACK. Mesurer la rétention par décalage contrôlé des commandes
   et essais de poll perdu sur banc isolé si nécessaire. Ne pas perturber les
   routeurs de production pour provoquer des pertes.
6. Mesurer via analyseur de puissance/shunt étalonné au **pack**, USB débranché,
   RX UART seul sans ré-alimentation parasite. Intégrer courant et tension à une
   cadence suffisante pour les pointes (viser ≥100 kéch/s, vérifier la bande passante)
   pendant 10 à 30 min au minimum par état, plus des cycles longs. Distinguer veille
   carte, association, SED sans commande, chaque commande et perte parent. Tester
   pack chargé/milieu/fin, température et chute de tension transitoire. Comparer
   avec/sans LED et régulateur caractérisé ; vérifier les durées de light-sleep.
7. Pour la latence, synchroniser traces Z2M et capture radio, ou corréler un événement
   d'envoi et un GPIO au callback sur le même analyseur logique ; conserver les
   identifiants ZCL. Le timestamp local du firmware seul ne mesure pas une latence
   depuis MQTT. Arrêter les logs périodiques pour la mesure énergétique finale.
8. Autoriser l'intégration CC1101 seulement après ces mesures et résolution du profil
   RF. Définir un seuil de latence STOP acceptable avec l'utilisateur avant de choisir
   T. Valider ensuite énergie/rafale, concurrence Zigbee/RMT et coupure durant compteur.
   Si le délai STOP ou la consommation échouent, envisager une carte dédiée ou une
   alimentation permanente plutôt qu'annoncer une autonomie non démontrée.

## Compilations et tests réellement exécutés

Voir `validation/build-results.json` et `validation/checks.md` pour les résultats,
versions et empreintes. Les logs bruts et binaires restent dans `build-validation/`
(ignoré par Git). Le script `tools/build-matrix.sh` ne contient que des opérations
de compilation, jamais `flash`, `erase-flash` ou accès série.

Une compilation ne valide ni association, ni maintien du parent, ni livraison,
ni courant de veille. Tous ces résultats matériels restent à acquérir.
