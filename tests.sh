#!/bin/bash
# Regression suite: feeds commands to mysh and compares combined stdout+stderr.
#   ./tests.sh            test ./mysh
#   SH=./mysh-asan ./tests.sh   test the sanitizer build
SH=${SH:-./mysh}
ROOT=$(cd "$(dirname "$0")" && pwd)
pass=0
fail=0

run() { (cd /tmp && printf '%s' "$1" | timeout -s KILL 20 "$ROOT/$SH" 2>&1 </dev/stdin); }

check() { # name, input, expected (exact)
  local got
  got=$(run "$2")
  if [ "$got" == "$3" ]; then pass=$((pass + 1)); else
    fail=$((fail + 1))
    echo "FAIL: $1"
    echo "  expected: $3" | head -5
    echo "  got:      $got" | head -5
  fi
}
has() { # name, input, expected substring
  local got
  got=$(run "$2")
  if [[ "$got" == *"$3"* ]]; then pass=$((pass + 1)); else
    fail=$((fail + 1))
    echo "FAIL: $1"
    echo "  expected to contain: $3"
    echo "  got: $got" | head -5
  fi
}
rm -f /tmp/mysh_*.txt /tmp/123

echo "-- basics --"
check "echo builtin" $'echo hello   world\n' 'hello world'
check "echo -n" $'echo -n hi\necho !\n' 'hi!'
check "quotes" $'echo "a   b" \'c  d\'\n' 'a   b c  d'
check "external cmd" $'/bin/echo ext\n' 'ext'
check "unknown cmd" $'nonexistent_cmd\n' 'mysh: nonexistent_cmd: No such file or directory'
check "cd" $'cd /usr\n/bin/pwd\n' '/usr'
check "cd missing" $'cd /nope\n' 'cd: /nope: No such file or directory'
check "cd updates PWD" $'cd /usr\ncd /tmp\necho $PWD\n' '/tmp'
check "cd -" $'cd /usr\ncd /tmp\ncd -\n' '/usr'

echo "-- redirection --"
check "redirect >" $'echo one > /tmp/mysh_a.txt\n/bin/cat /tmp/mysh_a.txt\n' 'one'
check "redirect >>" $'echo two >> /tmp/mysh_a.txt\n/bin/cat /tmp/mysh_a.txt\n' $'one\ntwo'
check "redirect <" $'wc -l < /tmp/mysh_a.txt\n' '2'
check "no spaces: x>f" $'echo x>/tmp/mysh_b.txt\n/bin/cat /tmp/mysh_b.txt\n' 'x'
check "< on file named 123" $'echo data > 123\ncat < 123\nrm 123\n' 'data'
check "2> file" $'ls /nonexistent_zz 2> /tmp/mysh_e.txt\nwc -l < /tmp/mysh_e.txt\n' '1'
check "2>> appends" $'ls /nonexistent_zz 2>> /tmp/mysh_e2.txt\nls /nonexistent_zz 2>> /tmp/mysh_e2.txt\nwc -l < /tmp/mysh_e2.txt\n' '2'
check "2>&1 into pipe" $'ls /nonexistent_zz 2>&1 | wc -l\n' '1'
check "&> file" $'ls /nonexistent_zz &> /tmp/mysh_f.txt\nwc -l < /tmp/mysh_f.txt\n' '1'
check "> f 2>&1 (order)" $'sh -c \'echo out; echo err >&2\' > /tmp/mysh_g.txt 2>&1\nwc -l < /tmp/mysh_g.txt\n' '2'
check "3> fd 3" $'sh -c \'echo fd3 >&3\' 3> /tmp/mysh_h.txt\ncat /tmp/mysh_h.txt\n' 'fd3'
check "missing input file" $'cat < /nonexistent_zz\n' 'mysh: /nonexistent_zz: No such file or directory'

echo "-- pipes --"
check "pipe" $'echo hello | tr a-z A-Z\n' 'HELLO'
check "3-stage pipe" $'printf "b\\na\\nb\\n" | sort | uniq\n' $'a\nb'
check "pipe + redirect" $'echo hi | tr a-z A-Z > /tmp/mysh_c.txt\ncat /tmp/mysh_c.txt\n' 'HI'
check "big pipe (no hang)" $'seq 1 200000 | wc -l\n' '200000'
check "pipe EOF closes" $'echo hi | cat | cat | cat\n' 'hi'
check "|& carries stderr" $'ls /nonexistent_zz |& wc -l\n' '1'
check "|& is per-link only" $'sh -c \'echo err >&2\' | wc -l\n' $'err\n0'

echo "-- lists --"
check "semicolon" $'echo one; echo two\n' $'one\ntwo'
check "&& runs on success" $'true && echo a\n' 'a'
check "&& skips on failure" $'false && echo a\n' ''
check "|| runs on failure" $'false || echo b\n' 'b'
check "|| skips on success" $'true || echo b\n' ''
check "false && a || b" $'false && echo a || echo b\n' 'b'
check "true && a || b" $'true && echo a || echo b\n' 'a'
check "true || a && b" $'true || echo a && echo b\n' 'b'
check "trailing ;" $'echo a;\n' 'a'
check "exit stops the list" $'exit 3 ; echo x\n' ''
has "bg then more cmds" $'sleep 0.1 & echo hi\nsleep 0.3\n' 'hi'

echo "-- variables --"
check "assign then use" $'A=5\necho $A\n' '5'
check "same-line expand" $'A=5; echo $A\n' '5'
check "\${VAR}" $'A=ab\necho ${A}c\n' 'abc'
check "single quotes" $'echo \'$HOME\'\n' '$HOME'
check "double quotes" $'A=x\necho "[$A]"\n' '[x]'
check "unset vanishes" $'echo $UNSET_Q foo\n' 'foo'
check "quoted unset stays" $'echo "$UNSET_Q" foo\n' ' foo'
check "\$? after false" $'false; echo $?\n' '1'
check "\$? after echo" $'echo hi; echo $?\n' $'hi\n0'
check "\$? in ||" $'false || echo $?\n' '1'
check "\$? after not found" $'nonexistent_cmd 2>/dev/null; echo $?\n' '127'
check "prefix assignment" $'FOO=bar printenv FOO\n' 'bar'
check "prefix doesn't stick" $'FOO=bar true\necho "[$FOO]"\n' '[]'
check "assign is exported" $'FOO=bar\nprintenv FOO\n' 'bar'
check "override reaches child" $'HOME=/tmp/zz\nprintenv HOME\n' '/tmp/zz'
check "two assignments" $'A=1 B=2\necho $A$B\n' '12'
check "assign + expand" $'A=hi\nB=$A-there\necho $B\n' 'hi-there'
check "pid is a number" $'echo $$ | grep -c "^[0-9][0-9]*$"\n' '1'

echo "-- errors --"
has "bad syntax |" $'| cat\n' "syntax error near '|'"
has "bad syntax >" $'echo hi >\n' "syntax error near '>'"
has "trailing |" $'echo hi |\n' 'syntax error'
has "trailing &&" $'echo hi &&\n' "syntax error near '&&'"
has "leading ;" $'; echo hi\n' "syntax error near ';'"
has "unterminated quote" $'echo "oops\n' 'unterminated quote'
has "ambiguous redirect" $'echo hi > $UNSET_Q\n' 'ambiguous redirect'

echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
