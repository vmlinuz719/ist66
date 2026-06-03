#!/bin/bash
# VIBASIC regression driver.  Usage: vbtest.sh <ppt-file>
# Runs a battery of programs/direct commands and prints just the VIBASIC output
# (strips the emulator banner and the cycles trailer) so two builds can be diffed.
PPT="$1"
H=/tmp/harness
run() {
    echo "### $1"
    shift
    "$H" "$PPT" "$@" 2>/dev/null \
        | sed -n '/WORDS FREE/,/^---- \[cycles/p' \
        | sed '1d;/^---- \[cycles/d'
}

# Integer arithmetic & precedence
run "int-arith"      "PRINT 2+3*4" "PRINT (2+3)*4" "PRINT 17\\5" "PRINT 17-5" "PRINT -7"
# Float arithmetic, mixed promotion
run "float-arith"    "PRINT 5/2" "PRINT 2+3.5" "PRINT 1.5E3" "PRINT 0.1+0.2" "PRINT 10/4"
# Float math functions
run "float-fns"      "PRINT SQR(2)" "PRINT EXP(1)" "PRINT LOG(EXP(1))" "PRINT 4*ATAN(1)"
run "trig"           "PRINT SIN(0)" "PRINT COS(0)" "PRINT TAN(0)" "PRINT ATAN(TAN(0.5))"
# Comparisons / relational
run "rel"            "PRINT 2.0>1.9" "PRINT 3=3" "PRINT 2<>3" "PRINT 5>=5" "PRINT 1<2"
# Integer functions
run "int-fns"        "PRINT INT(2.7)" "PRINT ABS(-5)" "PRINT ABS(-1.5)" "PRINT SGN(-3)" "PRINT SGN(4)"
# Bitwise / shift
run "bitwise"        "PRINT 12 AND 10" "PRINT 12 OR 3" "PRINT 1 SHL 4" "PRINT 256 SHR 2" "PRINT NOT 0"
# Strings
run "strings"        '10 A$="HELLO"' '20 B$="WORLD"' '30 PRINT A$+" "+B$' '40 PRINT LEN(A$)' 'RUN'
run "string-fns"     '10 A$="VIBASIC"' '20 PRINT LEFT$(A$,2)' '30 PRINT RIGHT$(A$,3)' '40 PRINT MID$(A$,2,3)' 'RUN'
run "string-num"     'PRINT STR$(42)' 'PRINT VAL("3.14")' 'PRINT CHR$(65)' 'PRINT ASC("A")'
run "str-float"      'PRINT STR$(-3.5)' 'PRINT "["+STR$(3.14159)+"]"' 'PRINT STR$(0.001)' 'PRINT LEN(STR$(2.5))'
# Control flow
run "for-next"       "10 FOR I=1 TO 5" "20 PRINT I" "30 NEXT I" "RUN"
run "for-step"       "10 S=0" "20 FOR I=2 TO 10 STEP 2" "30 S=S+I" "40 NEXT I" "50 PRINT S" "RUN"
run "for-neg"        "10 FOR I=3 TO 1 STEP -1" "20 PRINT I" "30 NEXT I" "RUN"
run "if-then"        "10 X=5" "20 IF X>3 THEN PRINT 99" "30 IF X<3 THEN PRINT 11" "RUN"
run "goto"           "10 X=0" "20 X=X+1" "30 IF X<3 THEN GOTO 20" "40 PRINT X" "RUN"
run "gosub"          "10 GOSUB 100" "20 PRINT 1" "30 END" "100 PRINT 2" "110 RETURN" "RUN"
# Arrays
run "arrays"         "10 DIM A(5)" "20 A(0)=3.14" "30 A(1)=A(0)*2" "40 PRINT A(1)" "RUN"
run "str-array"      '10 DIM S$(3)' '20 S$(0)="X"' '30 S$(1)="Y"' '40 PRINT S$(0)+S$(1)' 'RUN'
# LET / implicit let
run "let"            "10 LET A=10" "20 B=20" "30 PRINT A+B" "RUN"
# PEEK / POKE
run "peek-poke"      "10 POKE 1000,42" "20 PRINT PEEK(1000)" "RUN"
# REM / LIST round-trip
run "list"           "10 REM A LONGER COMMENT, WITH PUNCTUATION!" '20 PRINT "HI"' "30 FOR I=1 TO 3" "40 NEXT I" "LIST"
# INPUT
run "input-num"      "10 INPUT X" "20 PRINT X*2" "RUN" "21"
run "input-str"      '10 INPUT "NAME";N$' '20 PRINT "HI "+N$' "RUN" "DAVE"
# Errors
run "errors"         "PRINT 1/0" "10 FOR I=1.5 TO 3" "RUN" "GOTO 999"
# Nested loops + accumulation (temp-leak check)
run "nested"         "10 T=0" "20 FOR I=1 TO 3" "30 FOR J=1 TO 3" "40 T=T+0.5" "50 NEXT J" "60 NEXT I" "70 PRINT T" "RUN"
