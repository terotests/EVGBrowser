# EVG Browser

A small, safe web browser for children, written in [Ranger](https://github.com/terotests/Ranger)
on top of the EVG layout engine. It shows ordinary web pages — news, encyclopedia
articles, simple sites — without JavaScript, video, pop-ups or tracking, and only
from sites on an allowlist.

Because it is Ranger, the same source compiles to JavaScript (it runs inside a web
page, "browser in browser") and to C++ (the target EVG's SDL2 host is built on),
and EVG already has hosts for iOS and Android.

```
HTML ──► HtmlParser ──► CssCascade ──► HtmlToEvg ──► EVGLayout ──► EVGDisplayList ──► painter
          (tree)        (UA + page     (EVG elements,   (flex, grid,    (draw commands)   (SVG in a
                         sheets, real   styles written   text, images)                     tab; GL/
                         selectors)     inline)                                            native)
```

## Running it

It needs Node 20+ and nothing next to this repository. `npm install` brings
the Ranger compiler (the npm package `ranger-compiler`) and fetches the Ranger
packages the source uses — `evg`, `image`, `zip`, `componentengine`,
`ts_parser`, `core` — from the Ranger repository at the revision pinned in
`ranger.json` / `ranger.lock` into `vendor/ranger/` (`rgrc install -vendor`).
`RANGER_DIR=/path/to/Ranger` uses that checkout's compiler instead, for
trying a compiler change.

```sh
npm install            # once, and after ranger.json changes
npm run check          # headless checks, compiled to JavaScript
npm run check:cpp      # the same checks compiled to C++ (g++ or clang++)
npm run sdl            # the desktop browser, see below
npm run serve          # build web/dist and open http://localhost:8040/
```

`npm run serve` also starts a small fetch proxy on localhost, so every allowed site
works locally. It only fetches from the allowlist, GET only, text only, no cookies.

## Desktop (SDL2)

`native/sdl/evg_browser_sdl.cpp` puts the same program in a desktop window.
The Ranger source is compiled to C++ and included; the file adds only what a
desktop has: an SDL2 window and input, libcurl on worker threads for the
request queue, and a painter for the display list (rounded boxes and borders
as triangle geometry, text runs, `object-fit: cover`, a clip stack).

**Two libraries: SDL2 and libcurl** (which comes with macOS and every Linux).
Everything else is compiled in from single-file libraries in
`native/sdl/third_party/`: stb_truetype for type (the same faces measure the
text for the layout and draw it), stb_image for JPEG/PNG/GIF, Ranger's own WebP decoder
(`lib/image`), nanosvg for SVG, stb_image_write for screenshots. Colour emoji are read straight out of the
system's emoji font (the PNGs in Noto Color Emoji's CBDT table, or Apple
Color Emoji's sbix table). AVIF is not decoded, and image requests do not
ask for it.

```sh
# Debian/Ubuntu
sudo apt-get install libsdl2-dev libcurl4-openssl-dev fonts-dejavu-core
# macOS
brew install sdl2

npm run sdl                         # -> build/evg-browser
build/evg-browser                   # about:home
build/evg-browser https://yle.fi/uutiset
build/evg-browser --allow example.org,kids.example   # a parent adds sites
build/evg-browser --open https://www.hs.fi           # no allowlist at all (or --unsafe)
build/evg-browser about:demo --screenshot demo.png    # render headless, then exit
```

Mouse, wheel, drag-to-scroll, the mouse's back/forward buttons, Alt+←/→,
Ctrl+L (address field), Ctrl+R, Ctrl+V in fields, Ctrl+Q. Only `http` and
`https` are fetched, also after redirects; responses are capped at 8 MB;
pages in Latin-1 are converted to UTF-8. Fonts are looked for in the usual
Linux, macOS and Windows places, or in `EVG_FONT_DIR` (`sans.ttf`,
`sans-bold.ttf`, `mono.ttf`, `mono-bold.ttf`, `emoji.ttf`).

If clicks land in the wrong place (a Retina screen with an SDL build that
reports the mouse in pixels), `EVG_MOUSE_SCALE=2` fixes the ratio and
`EVG_DEBUG_INPUT=1` prints each mouse position the browser sees.

`.github/workflows/native.yml` builds it on Linux and renders two pages under
Xvfb on every push.

## The web demo and CORS

The GitHub Pages build (`.github/workflows/pages.yml`) publishes the same page. A
page on github.io can only read another site if that site allows it (CORS), and
most do not — **yle.fi does not**. So on Pages:

- the built-in pages and the bundled sample news page (`web/samples/`) work;
- Wikipedia works, through its CORS-enabled API (`web/fetchers.js`);
- other sites show an error page explaining this. They work when run locally
  with `npm run serve`, and in the desktop build, where there is no CORS.

To publish: merge to `main`, then set *Settings → Pages → Source* to
*GitHub Actions*. `?url=` opens a page directly, e.g. `…/?url=about:demo`.

## Safety

`src/SafetyPolicy.rgr`:

- **Allowlist.** Pages load only from listed hosts and their subdomains (yle.fi,
  wikipedia.org, a few science and library sites). A redirect off the list is
  refused too. Anything else shows a "this page is not allowed" page naming the
  site, so a parent can decide to add it (`setAllowedSites`). For grown-ups
  it can be switched off: `--open` or `EVG_OPEN=1` on the desktop,
  `?open=1` on the web page (with `EVG_OPEN=1 npm run serve` locally, so
  the proxy fetches any site too). The other rules stay on.
- **Scripts in a realm of their own.** A page's scripts run in Ranger's
  JavaScript interpreter (ComponentEngine) on a *copy* of the page, in a Web
  Worker on the web and in a child process on the desktop. They see a DOM
  (`src/js/prelude.js`) and nothing else: no network (`fetch` and
  `XMLHttpRequest` fail), no storage that outlives the page, no dialogs or
  windows, timers on a virtual clock. What they change comes back to the
  browser as DOM operations; a realm that runs longer than 8 s is stopped and
  the page stays as the server sent it. `?js=0` / `--no-js` turn scripts off.
- **No video, audio, iframes, plugins or canvas.** They are replaced by a short
  notice.
- **No overlays.** `position: fixed` boxes (cookie banners, chat bubbles,
  pop-ups) are not drawn, nor are absolute layers that cover their parent.
- **Forms**: only GET forms are submitted (search boxes work). Forms with
  password, e-mail or file fields, and POST forms, are disabled.
- Size limits on pages (4 MB), stylesheets (1.5 MB each, at most 32 per page; WordPress sites use 15–30) and on the
  parser (element count and depth), since input is whatever a server sends.

## How it works

EVG is a CSS-shaped layout engine with a deliberately small stylesheet (class
selectors only). EVG itself is not changed; the browser is a layer on top:

| File | Role |
| --- | --- |
| `src/HtmlParser.rgr` | Forgiving HTML parser: void and raw-text elements, implied end tags (`p`, `li`, `td`, …), entities, implied `html`/`head`/`body` |
| `src/CssParser.rgr` | Author stylesheets: type/class/id/attribute selectors, descendant and child combinators, `:not()`, `:first-child`…, `@media` evaluated against the viewport, `@supports` |
| `src/CssCascade.rgr` | The cascade: UA < author < inline < `!important`, specificity, inheritance, shorthands expanded, `em`/`rem`/`%` font sizes, custom properties and `var()` |
| `src/HtmlToEvg.rgr` | Styled tree → EVG elements, with each element's computed style written inline |
| `src/BrPage.rgr` | One document: its stylesheets, restyling at a width, image bookkeeping |
| `src/BrowserApp.rgr` | The browser: toolbar, address field, history, scrolling, links, forms. An `EvgApp`, plus `BrowserHost` (an `EvgHost`) |
| `src/BuiltinPages.rgr` | `about:home`, `about:demo`, `about:help`, the blocked and error pages — written in HTML and rendered like any page |
| `web/` | The web host: fetching, images, pointer/wheel/keyboard, SVG painting |

Things EVG does not have, and how they are handled:

- **Inline text.** EVG has no inline formatting context. A run of inline content
  becomes a flex row with `flex-wrap: wrap` and `align-items: baseline`, one text
  element per word. Words carry their own font, colour and link id, so a link is
  the set of words with its id, which is also what the hit test returns.
- **Shrink-to-fit boxes.** A container in a flex row fills the row in EVG; a box
  holding only a little text is drawn as one text element with the box's padding
  and background, and other row items become one-column flex containers, which
  EVG sizes to their content.
- **`margin: auto` centring** becomes a centring row around the box.
- **Percentage-width images** get their height after a first layout pass
  (`settleFluidImages`), from the picture's natural size.
- **`repeat(auto-fill, minmax(…))` grids** get a column count computed from the
  width.
- **Inline `<svg>`** is drawn as a picture: its markup goes into a
  `data:` URL, with `var()` and `currentColor` in its paint worked out
  from the page first (`CssCascade.bakeSvg`).
- **`float`** (an infobox, a picture with a caption): on a page at least
  560px wide the float gets a column of its own, and what follows it runs
  beside it up to an element that clears it or the next h1/h2; text does
  not wrap under the float. On a phone a float is a block.
- **`position: absolute`** inside a positioned parent is placed against
  that parent; edge-to-edge decoration layers are left out, and with the
  containing block further up the box stays in the flow.
- **Borders on one side only** are drawn as thin blocks (EVG borders are
  all-round).

## Scripts: the realm

```
browser                                   realm (Worker / child process)
  parse HTML, number nodes (DomTree)  ──►   parse the same HTML, same numbers
  fetch external scripts              ──►   run scripts in ComponentEngine
                                             against prelude.js's DOM
  apply ops, restyle, lay out         ◄──   every DOM change as an op:
                                             C/T create, A/R attributes,
                                             I insert, X remove, D text, W title
```

| File | Role |
| --- | --- |
| `src/DomTree.rgr` | node numbering, the ops as text, applying them |
| `src/js/JsRealm.rgr` | the realm: ComponentEngine plus the `__dom` bridge and selector matching |
| `src/js/prelude.js` | the DOM scripts see (generated into `JsPrelude.rgr` by `tools/gen-prelude.mjs`) |
| `web/realm-worker.js` | the Worker; `evg_realm.js` is loaded only when a page has scripts |
| `native/sdl/evg_browser_sdl.cpp` | `RealmProcess`: `evg-browser --realm` over pipes, killed on timeout |


## I/O is the host's job

`BrowserApp` never fetches anything. It queues requests; the host takes them
with `nextRequest()` (`"id\tkind\turl"`, kind `page`, `css` or `image`), fetches
them however its platform does, and answers with `deliver(id, status, finalUrl,
contentType, body)`, `imageLoaded(url, w, h)` or `imageFailed(url)`. This is what
lets the same code run in a tab (`fetch`) and in a native app (URLSession,
OkHttp, libcurl).

## Status

Done:
- the engine and the browser in Ranger; checks pass on JavaScript and C++;
- the web host and the Pages workflow;
- the SDL2 desktop host with libcurl, built and driven on Linux (Xvfb).

Not done yet:
- iOS and Android projects: `BrowserHost` is the class they would wrap (as
  EVG's other apps do), but the Swift/Kotlin glue and a fetcher are not written.
- The desktop build has been run on Linux only; macOS and Windows builds are
  untested.
- Text is not selectable; there is no italic or underline (EVG draws neither
  from its element tree); most pseudo-classes, `::before` /
  `::after` content and CSS background images are ignored. `calc()` is
  worked out when its terms are px, rem, pt or numbers (after `var()`),
  and left alone when they depend on the layout (%, em, vw).
- Scripts run once, at load: a click does not reach a page's own handlers
  yet, and modules (`<script type="module">`) do not run.

## License

AGPL-3.0-or-later, see `LICENSE`.
