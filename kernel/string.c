#include "types.h"
#include "x86.h"

/** @brief Set n bytes of memory to c */
void* memset(void* dst, int c, uint n)
{
    if ((int)dst % 4 == 0 && n % 4 == 0)
    {
        c &= 0xFF;
        stosl(dst, (c << 24) | (c << 16) | (c << 8) | c, n / 4);
    }
    else
        stosb(dst, c, n);
    return dst;
}

/** @brief Compare n bytes of memory */
int memcmp(const void* v1, const void* v2, uint n)
{
    const uchar* s1 = v1;
    const uchar* s2 = v2;
    while (n-- > 0)
    {
        if (*s1 != *s2)
            return *s1 - *s2;
        s1++, s2++;
    }

    return 0;
}

/** @brief Move n bytes of memory */
void* memmove(void* dst, const void* src, uint n)
{
    const char* s = src;
    char* d = dst;
    if (s < d && s + n > d)
    {
        s += n;
        d += n;
        while (n-- > 0)
            *--d = *--s;
    }
    else
        while (n-- > 0)
            *d++ = *s++;

    return dst;
}

// memcpy exists to placate GCC.  Use memmove.
/** @brief Copy n bytes of memory (use memmove) */
void* memcpy(void* dst, const void* src, uint n)
{
    return memmove(dst, src, n);
}

/** @brief Compare n characters of strings */
int strncmp(const char* p, const char* q, uint n)
{
    while (n > 0 && *p && *p == *q)
        n--, p++, q++;
    if (n == 0)
        return 0;
    return (uchar) * p - (uchar) * q;
}

/** @brief Copy n characters of string */
char* strncpy(char* s, const char* t, int n)
{
    char* os = s;
    while (n-- > 0 && (*s++ = *t++) != 0);
    while (n-- > 0)
        *s++ = 0;
    return os;
}

// Like strncpy but guaranteed to NUL-terminate.
/** @brief Copy string safely with NUL-termination */
char* safestrcpy(char* s, const char* t, int n)
{
    char* os = s;
    if (n <= 0)
        return os;
    while (--n > 0 && (*s++ = *t++) != 0);
    *s = 0;
    return os;
}

/** @brief Get length of string */
int strlen(const char* s)
{
    int n;

    for (n = 0; s[n]; n++);
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
