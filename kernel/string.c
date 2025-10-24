#include <stdarg.h>

#include "types.h"
#include "x86.h"

/** @brief Set n bytes of memory to c */
void *memset(void *dst, int c, u32 n)
{
    if ((int)dst % 4 == 0 && n % 4 == 0) {
        c &= 0xFF;
        stosl(dst, (c << 24) | (c << 16) | (c << 8) | c, n / 4);
    } else
        stosb(dst, c, n);
    return dst;
}

/** @brief Compare n bytes of memory */
int memcmp(const void *v1, const void *v2, u32 n)
{
    const u8 *s1 = v1;
    const u8 *s2 = v2;
    while (n-- > 0) {
        if (*s1 != *s2)
            return *s1 - *s2;
        s1++, s2++;
    }

    return 0;
}

/** @brief Move n bytes of memory */
void *memmove(void *dst, const void *src, u32 n)
{
    const char *s = src;
    char *d       = dst;
    if (s < d && s + n > d) {
        s += n;
        d += n;
        while (n-- > 0)
            *--d = *--s;
    } else
        while (n-- > 0)
            *d++ = *s++;

    return dst;
}

// memcpy exists to placate GCC.  Use memmove.
/** @brief Copy n bytes of memory (use memmove) */
void *memcpy(void *dst, const void *src, u32 n)
{
    return memmove(dst, src, n);
}

/** @brief Compare n characters of strings */
int strncmp(const char *p, const char *q, u32 n)
{
    while (n > 0 && *p && *p == *q)
        n--, p++, q++;
    if (n == 0)
        return 0;
    return (u8)*p - (u8)*q;
}

/** @brief Copy n characters of string */
char *strncpy(char *s, const char *t, int n)
{
    char *os = s;
    while (n-- > 0 && (*s++ = *t++) != 0) {}
    while (n-- > 0)
        *s++ = 0;
    return os;
}

// Like strncpy but guaranteed to NUL-terminate.
/** @brief Copy string safely with NUL-termination */
char *safestrcpy(char *s, const char *t, int n)
{
    char *os = s;
    if (n <= 0)
        return os;
    while (--n > 0 && (*s++ = *t++) != 0) {}
    *s = 0;
    return os;
}

/** @brief Get length of string */
int strlen(const char *s)
{
    int n;

    for (n = 0; s[n]; n++) {}
    return n;
}


bool starts_with(const char pre[static 1], const char str[static 1])
{
    return strncmp(pre, str, strlen(pre)) == 0;
}

char *strcat(char dest[static 1], const char src[static 1])
{
    char *d = dest;
    while (*d != '\0') {
        d++;
    }
    while (*src != '\0') {
        *d = *src;
        d++;
        src++;
    }
    *d = '\0';
    return dest;
}

void reverse(char *s)
{
    int i, j;

    for (i = 0, j = (int)strlen(s) - 1; i < j; i++, j--) {
        const int c = (u8)s[i];
        s[i]        = s[j];
        s[j]        = c;
    }
}

int itoa(int n, char *s)
{
    int sign;

    if ((sign = n) < 0) {
        n = -n;
    }
    int i = 0;
    do {
        s[i++] = n % 10 + '0';
    } while ((n /= 10) > 0);

    if (sign < 0) {
        s[i++] = '-';
    }

    s[i] = '\0';
    reverse(s);

    return i;
}

char *strchr(const char *s, int c)
{
    while (*s != '\0') {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return nullptr;
}

char *strtok(char *str, const char delim[static 1])
{
    static char *next = nullptr;
    // If str is provided, start from the beginning
    if (str != nullptr) {
        next = str;
    } else {
        // If no more tokens, return nullptr
        if (next == nullptr) {
            return nullptr;
        }
    }

    // Skip leading delimiters
    while (*next != '\0' && strchr(delim, *next) != nullptr) {
        next++;
    }

    // If end of string reached after skipping delimiters
    if (*next == '\0') {
        next = nullptr;
        return nullptr;
    }

    // Mark the start of the token
    char *start = next;

    // Find the end of the token
    while (*next != '\0' && strchr(delim, *next) == nullptr) {
        next++;
    }

    // If end of token is not the end of the string, terminate it
    if (*next != '\0') {
        *next = '\0';
        next++; // Move past the null terminator
    } else {
        // No more tokens
        next = nullptr;
    }

    return start;
}

int sscanf(const char *str, const char *format, ...)
{
    // Simple and limited implementation of sscanf
    va_list args;
    va_start(args, format);

    const char *s = str;
    const char *f = format;
    int assigned  = 0;

    while (*f && *s) {
        if (*f == '%') {
            f++;
            if (*f == 'd') {
                int *int_ptr = va_arg(args, int *);
                int value    = 0;
                int sign     = 1;

                // Skip whitespace
                while (*s == ' ' || *s == '\t' || *s == '\n') {
                    s++;
                }

                // Handle optional sign
                if (*s == '-') {
                    sign = -1;
                    s++;
                } else if (*s == '+') {
                    s++;
                }

                // Parse integer
                while (*s >= '0' && *s <= '9') {
                    value = value * 10 + (*s - '0');
                    s++;
                }
                *int_ptr = value * sign;
                assigned++;
            }
            if (*f == 's') {
                char *str_ptr = va_arg(args, char *);
                // Skip whitespace
                while (*s == ' ' || *s == '\t' || *s == '\n') {
                    s++;
                }
                // Copy string until next whitespace
                while (*s && *s != ' ' && *s != '\t' && *s != '\n') {
                    *str_ptr++ = *s++;
                }
                *str_ptr = '\0';
                assigned++;
            }
            f++;
        } else {
            if (*f != *s) {
                break; // Mismatch
            }
            f++;
            s++;
        }
    }

    va_end(args);
    return assigned;
}