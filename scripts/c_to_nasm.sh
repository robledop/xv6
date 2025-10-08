#!/bin/bash

# Translates a C header to a NASM header
# It replaces #pragma once with #ifndef, #define and #endif, but it may not cover all edge cases

# INPUT: A list of the required NASM headers

# The first argument is the directory where the C headers are located
HEADER_DIR=$1
shift # discard the first argument that was already stored in HEADER_DIR so that the remaining arguments are the NASM headers
# and can be iterated over

for NASM_HEADER in "$@"
do
    C_HEADER="${HEADER_DIR}/${NASM_HEADER%%.asm}.h"
    if [ "$C_HEADER" -nt "$NASM_HEADER" ]; then
        sed 's/\/\*/;/' "$C_HEADER" | # change start of block comments
        sed 's/\*\///'            | # change end of block comments
        sed 's/\/\//;/'           | # change start of line comments
        sed "s/'//g"               | # remove single quotes
        # replace #pragma once with #ifndef, #define
        sed 's/^[[:space:]]*#pragma[[:space:]]\+once/#ifndef '"_${NASM_HEADER%%.asm}"'\n#define '"_${NASM_HEADER%%.asm}"'\n/' |
        # convert enums into #define lines (simple parser)
        awk '
            function to_num(s,   s2, n, i, c, d, hexchars) {
                # strip whitespace
                gsub(/^[ \t\r\n]+|[ \t\r\n]+$/, "", s)
                # strip integer suffixes
                sub(/[uUlL]+$/, "", s)
                if (s ~ /^0[xX][0-9a-fA-F]+$/) {
                    s2 = substr(s, 3)
                    n = 0
                    hexchars = "0123456789abcdef"
                    s2 = tolower(s2)
                    for (i = 1; i <= length(s2); i++) {
                        c = substr(s2, i, 1)
                        d = index(hexchars, c) - 1
                        if (d < 0) { return 0 }
                        n = n * 16 + d
                    }
                    return n
                } else if (s ~ /^[0-9]+$/) {
                    return s + 0
                }
                return 0
            }
            BEGIN { in_enum=0; val=0 }
            {
                original=$0
                line=$0
                sub(/;.*/, "", line)              # remove anything after ; (comments converted earlier)
                sub(/\/\/.*/, "", line)          # remove // comments just in case
                gsub(/\/\*[^*]*\*\//, "", line) # remove /* */ comments on same line

                if (in_enum) {
                    if (line ~ /}/) { in_enum=0; next }
                    gsub(/[{};]/, "", line)
                    n=split(line, arr, ",")
                    for (i=1; i<=n; i++) {
                        token=arr[i]
                        gsub(/^[ \t\r\n]+|[ \t\r\n]+$/, "", token)
                        if (token == "") continue
                        if (token ~ /=/) {
                            split(token, kv, "=")
                            name=kv[1]; value=kv[2]
                            gsub(/^[ \t\r\n]+|[ \t\r\n]+$/, "", name)
                            gsub(/^[ \t\r\n]+|[ \t\r\n]+$/, "", value)
                            printf("#define %s %s\n", name, value)
                            v=value
                            val = to_num(v) + 1
                        } else {
                            name=token
                            printf("#define %s %d\n", name, val)
                            val++
                        }
                    }
                    next
                }

                if (line ~ /enum([[:space:]][a-zA-Z_][a-zA-Z0-9_]*)?[[:space:]]*\{/) {
                    in_enum=1
                    val=0
                    next
                }

                print original
            }
        ' |
        # keep only #define, #ifndef, #endif lines
        sed -n -e '/^[[:space:]]*#define/p' -e '/^[[:space:]]*#ifndef/p' -e '/^[[:space:]]*#endif/p' |
        sed 's/^[[:space:]]*#/%/'             > "${HEADER_DIR}/$NASM_HEADER" # change preprocessor directives and write to NASM header
    fi

    echo "" >> "${HEADER_DIR}/$NASM_HEADER" # ensure there is a newline at the end of the file
    # Add %endif at the end of the file if it is not present
    sed -i -e '$!b' -e '/%endif$/b' -e '$a%endif' "${HEADER_DIR}/$NASM_HEADER"
done
