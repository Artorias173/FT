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

/* Programmes en virgule flottante (variantes compilées avec l'unité VFP). */
#define VITA_FLOAT_TEST_COUNT 10
unsigned run_float_test(int index);
const char* float_test_name(int index);

/* Programmes NEON : intrinsics dans l'émulateur, référence scalaire en natif. */
#define VITA_NEON_TEST_COUNT 16
unsigned run_neon_test(int index);
const char* neon_test_name(int index);

#ifdef __cplusplus
}
#endif

#endif
