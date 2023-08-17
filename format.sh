#!/bin/bash
set -x

# Prepare clang-format

which clang-format || cp contrib/clang-format /usr/bin

find . \( -name '*.h' -or -name '*.cpp' \)  ! -path "./include*" -print0 |xargs -0 clang-format -i 

output=$(git diff|cat)
if [ -z "$output" ];then
    echo "clang format check succeeded"
else
    echo "Clang format check failed"
    echo "Fix diff as follows:"
    git diff|cat
    exit 1
fi
