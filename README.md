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

It needs a Ranger checkout next to this repository (`../Ranger`), or `RANGER_DIR`
pointing at one. Node 20+.

```sh
npm run check          # headless checks, compiled to JavaScript
npm run check:cpp      # the same checks compiled to C++ (g++ or clang++)
npm run serve          # build web/dist and open http://localhost:8040/
```

`npm run serve` also starts a small fetch proxy on localhost, so every allowed site
works locally. It only fetches from the allowlist, GET only, text only, no cookies.

## The web demo and CORS

The GitHub Pages build (`.github/workflows/pages.yml`) publishes the same page. A
page on github.io can only read another site if that site allows it (CORS), and
most do not — **yle.fi does not**. So on Pages:

- the built-in pages and the bundled sample news page (`web/samples/`) work;
- Wikipedia works, through its CORS-enabled API (`web/fetchers.js`);
- other sites show an error page explaining this. They work when run locally
  with `npm run serve`, and in a native build, where there is no CORS.

To publish: merge to `main`, then set *Settings → Pages → Source* to
*GitHub Actions*. `?url=` opens a page directly, e.g. `…/?url=about:demo`.

## Safety

`src/SafetyPolicy.rgr`:

- **Allowlist.** Pages load only from listed hosts and their subdomains (yle.fi,
  wikipedia.org, a few science and library sites). A redirect off the list is
  refused too. Anything else shows a "this page is not allowed" page naming the
  site, so a parent can decide to add it (`setAllowedSites`).
- **No scripts.** `<script>` is never run. Pages that build their content with
  JavaScript show what the server sent, including `<noscript>` content.
- **No video, audio, iframes, plugins or canvas.** They are replaced by a short
  notice.
- **No overlays.** `position: fixed` boxes (cookie banners, chat bubbles,
  pop-ups) are not drawn; other positioned boxes are laid out in flow.
- **Forms**: only GET forms are submitted (search boxes work). Forms with
  password, e-mail or file fields, and POST forms, are disabled.
- Size limits on pages (4 MB), stylesheets (1.5 MB, at most 8 per page) and on the
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
- **Borders on one side only** are drawn as thin blocks (EVG borders are
  all-round).

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
- the web host and the Pages workflow.

Not done yet:
- **Native windows.** The C++ build compiles and runs the checks, but there is no
  SDL2 window host or iOS/Android project for the browser yet. EVG's SDL2 GL
  painter lives in Ranger's AGPL `gallery/`, and a native build needs an HTTP
  client (e.g. libcurl) behind the request queue.
- Text is not selectable; there is no italic or underline (EVG draws neither
  from its element tree); `float`, `calc()`, most pseudo-classes, `::before` /
  `::after` content, CSS background images and inline SVG are ignored.
- Pages that render all their content with JavaScript appear mostly empty.

## License

MIT, see `LICENSE`.
