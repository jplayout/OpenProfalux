#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rx_capture.h"

#define TEST_CAPACITY KEELOQ_CAP_SYMBOLS
#define SMALL_CAPACITY 16

static const char EXPECTED[] =
      "100110100011110000101011"
      "110100101100011110000101"
      "101100011110000101";

static int failures;

static void check(bool ok, const char *name)
{
    printf(ok ? "  ok   %s\n" : "  FAIL %s\n", name);
    if (!ok) failures++;
}

static void fill_symbols(kq_sym_t *symbols, size_t count, uint16_t seed)
{
    for (size_t i = 0; i < count; i++) {
        symbols[i] = (kq_sym_t){
            .duration0 = (uint16_t)((seed + i) & 0x7FFF),
            .level0 = (uint16_t)(i & 1),
            .duration1 = (uint16_t)((seed + i + 1) & 0x7FFF),
            .level1 = (uint16_t)((i + 1) & 1),
        };
    }
}

static bool symbols_equal(const kq_sym_t *a, const kq_sym_t *b, size_t count)
{
    return memcmp(a, b, count * sizeof(*a)) == 0;
}

static size_t make_frame(kq_sym_t *symbols, size_t capacity)
{
    if (capacity < 68) return 0;
    symbols[0] = (kq_sym_t){ .duration0 = 430, .level0 = 1,
                             .duration1 = 4300, .level1 = 0 };
    for (size_t i = 0; i < 66; i++) {
        bool one = EXPECTED[i] == '1';
        symbols[i + 1] = (kq_sym_t){
            .duration0 = one ? 430 : 860, .level0 = 1,
            .duration1 = one ? 860 : 430, .level1 = 0,
        };
    }
    symbols[67] = (kq_sym_t){0};
    return 68;
}

static bool capture_decodes(const kq_sym_t *symbols, size_t count)
{
    char bits[80] = {0};
    return keeloq_decode(symbols, count, bits, sizeof(bits), NULL) == 66 &&
           strcmp(bits, EXPECTED) == 0;
}

int main(void)
{
    kq_sym_t storage[TEST_CAPACITY];
    kq_sym_t output[TEST_CAPACITY];
    kq_sym_t source[TEST_CAPACITY];
          kq_sym_t small_storage[SMALL_CAPACITY];
    rx_capture_t capture;
          rx_capture_t small_capture;
          size_t frame_count, captured;
    printf("Banc rx_capture\n");

    rx_capture_init(&capture, storage, TEST_CAPACITY);
    fill_symbols(source, 8, 10);
    check(rx_capture_append(&capture, source, 8, true), "T0 capture complete acceptee");
    check(rx_capture_snapshot(&capture, output, TEST_CAPACITY) == 8 &&
          symbols_equal(source, output, 8), "T0 capture sans fragmentation exacte");

    rx_capture_reset(&capture);
    frame_count = make_frame(source, TEST_CAPACITY);
    rx_capture_append(&capture, source, frame_count, true);
            captured = rx_capture_snapshot(&capture, output, TEST_CAPACITY);
    check(captured == frame_count && capture_decodes(output, captured),
          "T0 capture KeeLoq complete sans fragmentation decodee");

    rx_capture_reset(&capture);
    fill_symbols(source, 5, 20);
    rx_capture_append(&capture, source, 5, false);
    fill_symbols(source, 7, 25);
    rx_capture_append(&capture, source, 7, true);
    check(rx_capture_snapshot(&capture, output, TEST_CAPACITY) == 12,
          "T1 fragments accumules jusqu'a is_last");
    fill_symbols(source, 5, 20);
    fill_symbols(source + 5, 7, 25);
    check(symbols_equal(source, output, 12), "T1 ordre des fragments conserve");

    rx_capture_reset(&capture);
    fill_symbols(source, 10, 100);
    rx_capture_append(&capture, source, 6, false);
    rx_capture_append(&capture, source + 6, 4, true);
    check(rx_capture_snapshot(&capture, output, TEST_CAPACITY) == 10 &&
          symbols_equal(source, output, 10), "T2 trame a cheval sur une frontiere");

    rx_capture_reset(&capture);
      frame_count = make_frame(source, TEST_CAPACITY);
    size_t split = frame_count / 2;
    rx_capture_append(&capture, source, split, false);
    rx_capture_append(&capture, source + split, frame_count - split, true);
            captured = rx_capture_snapshot(&capture, output, TEST_CAPACITY);
    check(captured == frame_count && capture_decodes(output, captured),
          "T3 trame KeeLoq partagee a la frontiere decodee exacte");

    rx_capture_reset(&capture);
    fill_symbols(source, 6, 200);
    rx_capture_append(&capture, source, 6, false);
    check(rx_capture_snapshot(&capture, output, TEST_CAPACITY) == 0,
          "T4 fragment non final non publie");
    rx_capture_append(&capture, source, 0, true);
    check(rx_capture_snapshot(&capture, output, TEST_CAPACITY) == 6,
          "T4 is_last termine la capture");

    rx_capture_reset(&capture);
    fill_symbols(source, 8, 300);
    rx_capture_append(&capture, source, 8, false);
    memset(source, 0xA5, 8 * sizeof(*source));
    rx_capture_append(&capture, NULL, 0, true);
    fill_symbols(output, 8, 300);
    check(rx_capture_snapshot(&capture, storage, TEST_CAPACITY) == 8 &&
          symbols_equal(storage, output, 8), "T5 source reutilisee apres callback");

    rx_capture_reset(&capture);
    fill_symbols(source, 6, 400);
    rx_capture_append(&capture, source, 6, false);
    rx_capture_mark_event_lost(&capture);
    check(rx_capture_snapshot(&capture, output, TEST_CAPACITY) == 0 && capture.failed,
          "T6 notification terminale perdue invalide la transaction");

    rx_capture_init(&small_capture, small_storage, SMALL_CAPACITY);
    fill_symbols(source, SMALL_CAPACITY, 500);
    rx_capture_append(&small_capture, source, SMALL_CAPACITY, false);
    fill_symbols(source, 5, 516);
    rx_capture_append(&small_capture, source, 5, true);
    check(small_capture.overflow && small_capture.count == SMALL_CAPACITY,
          "T7 saturation reste bornee et declaree");
    check(rx_capture_snapshot(&small_capture, output, TEST_CAPACITY) == 0,
          "T7 capture saturee refusee au decodeur");

    rx_capture_reset(&capture);
    frame_count = make_frame(source, TEST_CAPACITY);
    rx_capture_append(&capture, source, frame_count, true);
    captured = rx_capture_snapshot(&capture, output, TEST_CAPACITY);
    bool first_frame_ok = captured == frame_count && capture_decodes(output, captured);
    rx_capture_reset(&capture);
    frame_count = make_frame(source, TEST_CAPACITY);
    rx_capture_append(&capture, source, frame_count, true);
    captured = rx_capture_snapshot(&capture, output, TEST_CAPACITY);
    check(first_frame_ok && captured == frame_count && capture_decodes(output, captured),
          "T8 deux trames KeeLoq successives decodees independamment");

    rx_capture_reset(&capture);
    fill_symbols(source, TEST_CAPACITY, 800);
    rx_capture_append(&capture, source, TEST_CAPACITY, false);
    fill_symbols(source, 4, 816);
    rx_capture_append(&capture, source, 4, false);
    fill_symbols(source, 4, 820);
    rx_capture_append(&capture, source, 4, true);
    check(capture.overflow && rx_capture_snapshot(&capture, output, TEST_CAPACITY) == 0,
          "T9 consommateur retarde, capture tronquee refusee");

    rx_capture_reset(&capture);
    check(!rx_capture_append(&capture, NULL, 1, false) && capture.failed,
          "T10 evenement invalide signale et recuperable");
    rx_capture_reset(&capture);
    fill_symbols(source, 3, 900);
    rx_capture_append(&capture, source, 3, true);
    check(rx_capture_snapshot(&capture, output, TEST_CAPACITY) == 3 &&
          symbols_equal(source, output, 3), "T10 reinitialisation apres erreur");

    printf(failures ? "\n%d test(s) en echec\n" : "\nTout passe\n", failures);
    return failures ? 1 : 0;
}