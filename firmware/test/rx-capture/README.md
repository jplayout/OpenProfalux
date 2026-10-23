# Banc hote de la collecte RMT RX

Teste `rx_capture.c` et `keeloq_decode.c` sans ESP-IDF ni materiel. Les fragments
sont injectes comme le ferait le callback public RMT : chaque appel fournit son propre pointeur et
`is_last` indique la fin de la transaction. Le collecteur copie les symboles a
l'appel et rejette toute transaction incomplete ou saturee.

## Lancer

```bash
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -g \
   -I main -I ../../main main/test_main.c ../../main/rx_capture.c \
   ../../main/keeloq_decode.c \
   -o rx_capture_host_test
./rx_capture_host_test
```

Ou avec CMake : `cmake -B build && cmake --build build && ./build/rx_capture_host_test`.

Le banc couvre la capture simple et fragmentee, une trame KeeLoq partagee a la
frontiere puis decodee, la fin explicite, la reutilisation du buffer source, une
notification terminale perdue, le rejet du debordement, des captures successives
et un consommateur retarde.