#!/bin/sh
# cmd.sh: Run clang-query to report unused return values.

set -e

# When --dump, print the AST of matching syntax.
if [ "x$1" = "x--dump" ]; then
    dump="set output dump"
    shift
fi

query='m
  callExpr(
    isExpansionInMainFile(),
    hasParent(anyOf(
      compoundStmt(),
      ifStmt(hasCondition(expr().bind("cond"))),
      whileStmt(hasCondition(expr().bind("cond"))),
      doStmt(hasCondition(expr().bind("cond")))
    )),
    unless(hasType(voidType())),
    unless(isTypeDependent()),
    unless(cxxOperatorCallExpr()),
    unless(callee(namedDecl(anyOf(
      hasName("memset"),
      hasName("setlength"),
      hasName("flags"),
      hasName("width"),
      hasName("__builtin_memcpy")
    )))),
    unless(equalsBoundNode("cond")))'

clang-query -c="$dump" -c="$query" "$@"
