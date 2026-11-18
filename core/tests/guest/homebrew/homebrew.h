/* Calculs du homebrew de test : exécutés dans l'émulateur et en natif. */
#ifndef VITA_TEST_HOMEBREW_H
#define VITA_TEST_HOMEBREW_H

#ifdef __cplusplus
extern "C" {
#endif

#define HB_VALUE_COUNT 7
unsigned hb_value(int index);

#ifdef __cplusplus
}
#endif

#endif
