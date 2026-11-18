/* Drop LIBGL_KOPPER_DISABLE from the process environment before Mesa reads it.
 * Same effect as DroidDeck's venus.c preload (a7bd6ed29), edits environ directly
 * (no setenv/unsetenv, which the guest's guard refuses). Gated on BL_OPEN_DRIVER. */
extern char **environ;
static int eq(const char *a, const char *b, int n) { while (n--) if (*a++ != *b++) return 0; return 1; }
static int has(char **e, const char *k, int n) { for (; e && *e; e++) if (eq(*e, k, n)) return 1; return 0; }
__attribute__((constructor)) static void nokopper(void) {
    static const char name[] = "LIBGL_KOPPER_DISABLE=";
    static const char gate[] = "BL_OPEN_DRIVER=";
    if (!environ || !has(environ, gate, sizeof gate - 1)) return;
    char **out = environ;
    for (char **in = environ; *in; in++)
        if (!eq(*in, name, sizeof name - 1)) *out++ = *in;
    *out = 0;
}
