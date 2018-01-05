#ifndef _SYNTAX_BRAIN_H
#define _SYNTAX_BRAIN_H

char *brain_extensions[] = {".brain", ".b", ".bf", NULL};
char *brain_keywords[] = {
        "+", "-", "#", ".", ",", ">", "<", "*", "[", "]", "{", "}",
        "$", "/", "%", "!", "?", ":", ";", "^", NULL
};

// Anything else than those keywords are considered comments in Brain.

#define BRAIN_syntax {   \
    brain_extensions,    \
    brain_keywords,      \
    "",                  \
    "",                  \
    "",                  \
    HL_HIGHLIGHT_STRINGS \
}

#endif