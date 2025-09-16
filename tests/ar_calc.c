/*
 * areole - the maths and custom-property corpus.
 * SPDX-License-Identifier: MIT
 *
 * What `calc()`, the maths functions and `var()` compute to, checked against
 * a browser rather than against my arithmetic.
 *
 *     ar_calc                                areole's computed values
 *     ar_calc --html > tests/calc.html       the browser twin
 *     python tools/compare_computed.py --run ./build/ar_calc.exe \
 *            tests/calc.html
 *
 * 0.4.3's second acceptance criterion. The machinery is in ar_computed.h and
 * the cases are generated -- see tools/gen_calc_corpus.py for why, and for
 * what is deliberately left out.
 *
 * ------------------------------------------------------------------------
 * Why a browser is the authority for arithmetic, again
 *
 * The same argument the units corpus makes, and it held twice more here.
 * `calc()` is a closed calculation with no rendering in it, and it shipped
 * three bugs that reasoning did not catch: a term loop that ate the
 * whitespace a sum needs, a refused expression that left its own text for the
 * declaration parser to misread, and a `--name` rule that was thrown away
 * before it reached the table because its property set was empty.
 *
 * None of those is arithmetic. All three produced a number.
 *
 * ------------------------------------------------------------------------
 * The rows that are about failing
 *
 * Eight cases end in `bad-` and report `height` rather than `width`. Each
 * writes an expression that must be refused and a height beside it that must
 * survive -- so the row proves two things at once: that the bad declaration
 * was dropped, and that dropping it cost only itself.
 *
 * That second half is not decoration. A refused value used to leave the
 * scanner standing inside the expression, and the declaration parser recovers
 * by reading the next value it can find: `calc(50% - 10px)` came out ten
 * pixels, and the declaration after it was fine. Only a witness beside the
 * failure can tell the two apart.
 */
#include "ar_computed.h"
#include "ar_calc_cases.h"

int main(int argc, char **argv)
{
    return ar__corpus_main(argc, argv, CASES, CASE_N, "calc and custom properties", "ar_calc", 0);
}
