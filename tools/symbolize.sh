#!/bin/sh
# Symbolize "[CRASH]   exe+OFFSET" lines from a crash log: tools/symbolize.sh run_err.txt
L=$(ls -d /c/Users/*/AppData/Local/Microsoft/WinGet/Packages/MartinStorsjo.LLVM-MinGW*/llvm-mingw-*/bin 2>/dev/null | head -1)
grep -o "exe+[0-9a-f]*" "$1" | sed 's/exe+/0x/' | while read off; do
  a=$(printf "0x%x" $((off + 0x140000000)))
  echo "$off $("$L/llvm-symbolizer" --obj=gt.exe --no-inlines -f $a | head -2 | tr '\n' ' ')"
done
