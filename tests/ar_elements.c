/*
 * areole - the user-agent stylesheet corpus.
 * SPDX-License-Identifier: MIT
 *
 * What every element's defaults compute to, checked against a browser rather
 * than against my reading of the rendering section.
 *
 *     ar_elements                                 areole's computed values
 *     ar_elements --html > tests/elements.html    the browser twin
 *     python tools/compare_computed.py --run ./build/ar_elements.exe \
 *            tests/elements.html
 *
 * 0.9.1's first acceptance criterion. The machinery is in ar_computed.h, which
 * says why this is a corpus of computed values and not of rectangles; what is
 * here is one case per element the stylesheet names.
 *
 * GENERATED from the selectors in src/ar_ua_css.c. An element added to the
 * sheet and not to this file is an element with no case, which is the failure
 * this corpus exists to make impossible -- so regenerate it rather than
 * editing it, and CI diffs the result.
 *
 * ------------------------------------------------------------------------
 * What each element is wrapped in
 *
 * A `<td>` outside a table is not a table cell in either engine, and two boxes
 * that are wrong in the same way prove nothing. Everything with a required
 * parent gets one. The document's own structure -- html, head, body -- and the
 * frameset family are left out: they cannot be put inside a body and compared,
 * because a body-context fragment parser drops them.
 *
 * ------------------------------------------------------------------------
 * Nine properties, not one
 *
 * A default is a handful of declarations at once and asking for them one case
 * at a time would mean parsing the same fragment nine times. `display` first
 * because it is the one that decides whether anything else means anything: a
 * `<p>` that came out `inline` has the right margins on the wrong kind of box.
 */
#include "ar_computed.h"

#define PROPS                                                                                      \
    "display margin-top margin-bottom margin-left padding-left font-size font-weight font-style "  \
    "text-align"

static const ar__case CASES[] = {
    {"a", "", "<a id=\"x\">t</a>", PROPS},
    {"abbr", "", "<abbr id=\"x\">t</abbr>", PROPS},
    {"address", "", "<address id=\"x\">t</address>", PROPS},
    {"area", "", "<map><area id=\"x\"></map>", PROPS},
    {"article", "", "<article id=\"x\">t</article>", PROPS},
    {"aside", "", "<aside id=\"x\">t</aside>", PROPS},
    {"audio", "", "<audio id=\"x\">t</audio>", PROPS},
    {"b", "", "<b id=\"x\">t</b>", PROPS},
    {"base", "", "<base id=\"x\">", PROPS},
    {"bdi", "", "<bdi id=\"x\">t</bdi>", PROPS},
    {"bdo", "", "<bdo id=\"x\">t</bdo>", PROPS},
    {"big", "", "<big id=\"x\">t</big>", PROPS},
    {"blockquote", "", "<blockquote id=\"x\">t</blockquote>", PROPS},
    {"br", "", "<br id=\"x\">", PROPS},
    {"button", "", "<button id=\"x\">t</button>", PROPS},
    {"canvas", "", "<canvas id=\"x\">t</canvas>", PROPS},
    {"caption", "", "<table><caption id=\"x\">t</caption></table>", PROPS},
    {"center", "", "<center id=\"x\">t</center>", PROPS},
    {"cite", "", "<cite id=\"x\">t</cite>", PROPS},
    {"code", "", "<code id=\"x\">t</code>", PROPS},
    {"col", "", "<table><colgroup><col id=\"x\"></colgroup></table>", PROPS},
    {"colgroup", "", "<table><colgroup id=\"x\">t</colgroup></table>", PROPS},
    {"data", "", "<data id=\"x\">t</data>", PROPS},
    {"datalist", "", "<form><datalist id=\"x\">t</datalist></form>", PROPS},
    {"dd", "", "<dl><dd id=\"x\">t</dd></dl>", PROPS},
    {"del", "", "<del id=\"x\">t</del>", PROPS},
    {"details", "", "<details id=\"x\">t</details>", PROPS},
    {"dfn", "", "<dfn id=\"x\">t</dfn>", PROPS},
    {"dialog", "", "<dialog id=\"x\">t</dialog>", PROPS},
    {"dir", "", "<dir id=\"x\">t</dir>", PROPS},
    {"div", "", "<div id=\"x\">t</div>", PROPS},
    {"dl", "", "<dl id=\"x\">t</dl>", PROPS},
    {"dt", "", "<dl><dt id=\"x\">t</dt></dl>", PROPS},
    {"em", "", "<em id=\"x\">t</em>", PROPS},
    {"embed", "", "<embed id=\"x\">", PROPS},
    {"fieldset", "", "<fieldset id=\"x\">t</fieldset>", PROPS},
    {"figcaption", "", "<figure><figcaption id=\"x\">t</figcaption></figure>", PROPS},
    {"figure", "", "<figure id=\"x\">t</figure>", PROPS},
    {"footer", "", "<footer id=\"x\">t</footer>", PROPS},
    {"form", "", "<form id=\"x\">t</form>", PROPS},
    {"h1", "", "<h1 id=\"x\">t</h1>", PROPS},
    {"h2", "", "<h2 id=\"x\">t</h2>", PROPS},
    {"h3", "", "<h3 id=\"x\">t</h3>", PROPS},
    {"h4", "", "<h4 id=\"x\">t</h4>", PROPS},
    {"h5", "", "<h5 id=\"x\">t</h5>", PROPS},
    {"h6", "", "<h6 id=\"x\">t</h6>", PROPS},
    {"header", "", "<header id=\"x\">t</header>", PROPS},
    {"hgroup", "", "<hgroup id=\"x\">t</hgroup>", PROPS},
    {"hr", "", "<hr id=\"x\">", PROPS},
    {"i", "", "<i id=\"x\">t</i>", PROPS},
    {"iframe", "", "<iframe id=\"x\">t</iframe>", PROPS},
    {"img", "", "<img id=\"x\">", PROPS},
    {"input", "", "<input id=\"x\">", PROPS},
    {"ins", "", "<ins id=\"x\">t</ins>", PROPS},
    {"kbd", "", "<kbd id=\"x\">t</kbd>", PROPS},
    {"label", "", "<label id=\"x\">t</label>", PROPS},
    {"legend", "", "<fieldset><legend id=\"x\">t</legend></fieldset>", PROPS},
    {"li", "", "<ul><li id=\"x\">t</li></ul>", PROPS},
    {"link", "", "<link id=\"x\">", PROPS},
    {"listing", "", "<listing id=\"x\">t</listing>", PROPS},
    {"main", "", "<main id=\"x\">t</main>", PROPS},
    {"map", "", "<map id=\"x\">t</map>", PROPS},
    {"mark", "", "<mark id=\"x\">t</mark>", PROPS},
    {"marquee", "", "<marquee id=\"x\">t</marquee>", PROPS},
    {"menu", "", "<menu id=\"x\">t</menu>", PROPS},
    {"meta", "", "<meta id=\"x\">", PROPS},
    {"meter", "", "<meter id=\"x\">t</meter>", PROPS},
    {"nav", "", "<nav id=\"x\">t</nav>", PROPS},
    {"noscript", "", "<noscript id=\"x\">t</noscript>", PROPS},
    {"object", "", "<object id=\"x\">t</object>", PROPS},
    {"ol", "", "<ol id=\"x\">t</ol>", PROPS},
    {"optgroup", "", "<select><optgroup id=\"x\">t</optgroup></select>", PROPS},
    {"option", "", "<select><option id=\"x\">t</option></select>", PROPS},
    {"output", "", "<output id=\"x\">t</output>", PROPS},
    {"p", "", "<p id=\"x\">t</p>", PROPS},
    {"param", "", "<object><param id=\"x\"></object>", PROPS},
    {"picture", "", "<picture id=\"x\">t</picture>", PROPS},
    {"plaintext", "", "<plaintext id=\"x\">t</plaintext>", PROPS},
    {"pre", "", "<pre id=\"x\">t</pre>", PROPS},
    {"progress", "", "<progress id=\"x\">t</progress>", PROPS},
    {"q", "", "<q id=\"x\">t</q>", PROPS},
    {"rp", "", "<ruby><rp id=\"x\">t</rp></ruby>", PROPS},
    {"rt", "", "<ruby><rt id=\"x\">t</rt></ruby>", PROPS},
    {"ruby", "", "<ruby id=\"x\">t</ruby>", PROPS},
    {"s", "", "<s id=\"x\">t</s>", PROPS},
    {"samp", "", "<samp id=\"x\">t</samp>", PROPS},
    {"script", "", "<script id=\"x\">t</script>", PROPS},
    {"search", "", "<search id=\"x\">t</search>", PROPS},
    {"section", "", "<section id=\"x\">t</section>", PROPS},
    {"select", "", "<select id=\"x\">t</select>", PROPS},
    {"slot", "", "<slot id=\"x\">t</slot>", PROPS},
    {"small", "", "<small id=\"x\">t</small>", PROPS},
    {"source", "", "<source id=\"x\">", PROPS},
    {"span", "", "<span id=\"x\">t</span>", PROPS},
    {"strike", "", "<strike id=\"x\">t</strike>", PROPS},
    {"strong", "", "<strong id=\"x\">t</strong>", PROPS},
    {"style", "", "<style id=\"x\">t</style>", PROPS},
    {"sub", "", "<sub id=\"x\">t</sub>", PROPS},
    {"summary", "", "<details><summary id=\"x\">t</summary></details>", PROPS},
    {"sup", "", "<sup id=\"x\">t</sup>", PROPS},
    {"table", "", "<table id=\"x\">t</table>", PROPS},
    {"tbody", "", "<table><tbody id=\"x\">t</tbody></table>", PROPS},
    {"td", "", "<table><tr><td id=\"x\">t</td></tr></table>", PROPS},
    {"textarea", "", "<textarea id=\"x\">t</textarea>", PROPS},
    {"tfoot", "", "<table><tfoot id=\"x\">t</tfoot></table>", PROPS},
    {"th", "", "<table><tr><th id=\"x\">t</th></tr></table>", PROPS},
    {"thead", "", "<table><thead id=\"x\">t</thead></table>", PROPS},
    {"time", "", "<time id=\"x\">t</time>", PROPS},
    {"title", "", "<title id=\"x\">t</title>", PROPS},
    {"tr", "", "<table><tr id=\"x\">t</tr></table>", PROPS},
    {"track", "", "<track id=\"x\">", PROPS},
    {"tt", "", "<tt id=\"x\">t</tt>", PROPS},
    {"u", "", "<u id=\"x\">t</u>", PROPS},
    {"ul", "", "<ul id=\"x\">t</ul>", PROPS},
    {"var", "", "<var id=\"x\">t</var>", PROPS},
    {"video", "", "<video id=\"x\">t</video>", PROPS},
    {"wbr", "", "<wbr id=\"x\">", PROPS},
    {"xmp", "", "<xmp id=\"x\">t</xmp>", PROPS},
};

#define CASE_N ((int)(sizeof CASES / sizeof CASES[0]))

int main(int argc, char **argv)
{
    return ar__corpus_main(argc, argv, CASES, CASE_N, "user-agent stylesheet", "ar_elements");
}
