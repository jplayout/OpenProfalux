# Prototype Zigbee autonome — ESP32-C6FH4

Le [rapport de faisabilité](FEASIBILITY.md) décrit les versions, les limites
énergétiques, les adaptations radio et le plan d'essai Zigbee2MQTT.

Prototype indépendant d'OpenProfalux IDF 6.1 : **IDF 5.5.4 + esp-zigbee-lib 2.0.5**,
Sleepy End Device, light-sleep automatique, quatre endpoints Window Covering
10..13. OPEN/CLOSE/STOP sont journalisés uniquement. Aucun CC1101, Wi-Fi ou client
MQTT applicatif n'est initialisé. Les composants réseau transitifs de la pile SDK
ne constituent pas une connexion Wi-Fi ; le projet ne configure aucun accès Wi-Fi.
Aucun attribut de position courante ni état optimiste n'est exposé.

## Compiler sans matériel

Depuis la racine du dépôt, avec Podman :

```sh
podman run --rm --network=host -v "$PWD":/project:Z \
  -w /project/firmware-zigbee docker.io/espressif/idf:v5.5.4 \
  bash tools/build-matrix.sh
```

Les variantes 1000/2000/5000 ms sont compilées successivement ; les résultats et
empreintes sont dans `validation/build-results.json`, les binaires et logs dans
`build-validation/`. La première exécution télécharge la dépendance du registre.
Le firmware utilise une file bornée et refuse toute erreur NVS sans effacement.
Les credentials réseau sont conservés dans `nvs` après les futurs essais matériels.

Pour un build simple, dans l'environnement IDF 5.5.4 :

```sh
idf.py -DIDF_TARGET=esp32c6 build
```

`menuconfig` > OpenProfalux Zigbee simulation permet de choisir le polling ; le
défaut est 1 s. La cadence réelle, le sommeil et le retour réseau sont à vérifier
sur matériel. Aucun flash n'est prévu dans cette étape.

## Convertisseur et test hors ligne

Le convertisseur ESM `z2m/profalux-gateway.mjs` expose les commandes `state_volet_1`
à `state_volet_4`, valeurs OPEN/CLOSE/STOP, SET uniquement. L'installation future
sur Z2M est détaillée dans le rapport ; elle n'a pas été exécutée.

Pour reproduire le test dans un répertoire temporaire avec Node 22 :

```sh
mkdir -p /tmp/opf-z2m-test
cp firmware-zigbee/z2m/*.mjs /tmp/opf-z2m-test/
cd /tmp/opf-z2m-test
npm init -y
npm install --save-exact zigbee-herdsman-converters@26.123.0
node profalux-gateway.test.mjs
```

Ce test simule l'endpoint herdsman et vérifie les 12 commandes, les exposes,
le refus des valeurs invalides et les erreurs de livraison. Il ne parle à aucun
coordinateur ni broker.

## Variante USB de diagnostic

La [procédure USB pour Bazzite](USB-DIAGNOSTIC.md) fournit la compilation, les
binaires, la table de partitions et les commandes de flash/monitoring à exécuter
ultérieurement. L'overlay `sdkconfig.usb-debug.defaults` utilise la console USB
Serial/JTAG et désactive le light-sleep automatique dans l'application ; le build
et le sdkconfig restent dans `build-usb-debug/`. Le SED conserve RX-off, quatre
endpoints et le polling de 1 s. Cette variante ne sert pas aux mesures sur piles.
Les [résultats détaillés](validation/usb-debug-checks.md) incluent les tests existants
et les deux échecs KeeLoq reproduits sur le commit de départ.
