# Banc hôte du port CC1101 et du service de commandes

Depuis la racine du dépôt (aucun matériel, ASan + UBSan activés) :

```sh
podman run --rm --network=none -v "$PWD":/project:ro,z \
  docker.io/espressif/idf:v5.5.4 bash -c \
  'cmake -S /project/firmware-zigbee/test/radio -B /tmp/radio-test && cmake --build /tmp/radio-test && ctest --test-dir /tmp/radio-test --output-on-failure && /tmp/radio-test/pfx_radio_tests'
```

Le bus SPI est simulé au niveau transaction. Le banc exécute le vrai port portable,
la vraie file bornée utilisée par l'adaptateur Zigbee et le vrai service de mapping
et d'apprentissage. Les callbacks ESP-IDF (SPI GPIO/RMT et NVS physique) ne tournent
pas sur hôte. Le mock de persistance vérifie l'absence de publication RAM sur échec
et la lecture d'un snapshot ancien ou nouveau ; il ne simule pas les secteurs flash
internes de NVS. La résistance aux coupures réelles reste à valider.

Couverture : module absent (MISO haut/bas, version inconnue), échec d'ouverture SPI,
erreur de lecture, timeout de disponibilité/IDLE/RX, relecture des registres,
veille/réveil, TX bloqué dans tous les modes, simulation avec radio absente,
données manquantes, douze associations endpoint/commande, commandes rapprochées,
saturation, FIFO et bouclage de file, capture confirmée, délai d'apprentissage,
CRC/schéma/taille/caractères invalides, sauvegarde refusée et clear explicite.
Une assertion interdit tout strobe STX dans le faux bus.

Les tests historiques `firmware/test/rx-capture`, `rx-confirm` et `keeloq` vérifient
séparément la collecte et le décodage des impulsions utilisés par l'adaptateur.
Le test `sim/test_keeloq.c` (algorithme et dérivation) reste distinct : deux de ses
vecteurs échouaient déjà avant cette intégration.
