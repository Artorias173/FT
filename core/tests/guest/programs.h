/* Programmes de test exécutés à la fois nativement et dans l'émulateur. */
#ifndef VITA_TEST_PROGRAMS_H
#define VITA_TEST_PROGRAMS_H

#ifdef __cplusplus
extern "C" {
#endif

#define VITA_TEST_COUNT 14

/* Renvoie le résultat du test numéro `index` (entre 0 et VITA_TEST_COUNT - 1). */
unsigned run_test(int index);
const char* test_name(int index);

#ifdef __cplusplus
}
#endif

#endif
