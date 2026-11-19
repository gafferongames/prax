// Studio's page view: a document of the web, HTML or Markdown, rendered by lib/html in a Studio Kit tree.
//
// The view loads a document from bytes that its host gives, at a URL that the host chooses, and shows it as content of
// the Kit tree's flow: a replaced element as wide as the view, as tall as the document and at least as tall as the
// view, in a box that scrolls it. It is one more node of the tree, so the tree clips, scrolls and covers it as it does
// any other. A Markdown document is the HTML that lib/markdown makes of it, in lib/html's style sheet for Markdown, in
// the color scheme of the user's preferences.
//
// The document takes the input of a document in a browser's viewport: the pointer over it, the wheel over it, and the
// keys and text while it has focus, which a press on it gives it. The wheel and the keys scroll the document's own
// scroll containers first, and what they do not take scrolls the box that shows the document (prHtmlViewOverscrolled).
//
// The document fetches what it names through the host's fetcher: each image, style sheet or other resource, by its URL
// resolved against the document's, and the document of a link that the user follows, as a navigation, which the host
// answers as it chooses, such as by opening it elsewhere. The document runs no script. One thread uses a view.

#pragma once

#include "pr/blob.h"
#include "pr/compress.h"
#include "pr/fetch.h"
#include "pr/image.h"
#include "pr/kit.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/text_font.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A document in a Kit tree.
typedef struct PrPageView PrPageView;

// What a view renders with and fetches through. The kit, the font's set, the shaper, the codecs and the fetcher outlive
// the view.
typedef struct PrPageViewDesc {
    PrKit* kit;
    PrTextFont* font;                   // the document's default font, which the view holds a reference to
    PrTextShaper* shaper;
    const PrImageCodecSet* PR_NULLABLE images;   // decodes the document's images; with none, they draw nothing
    const PrCompressCodecSet* PR_NULLABLE compress;
    PrFetchNetwork* fetcher;
} PrPageViewDesc;

// Creates a view of no document, or frees one.
PrPageView* prPageViewCreate(const PrPageViewDesc* desc);
void prPageViewDestroy(PrPageView* PR_NULLABLE view);

// Loads `bytes`, a document of the media type `contentType` (text/html or text/markdown, with a charset parameter if
// they are not UTF-8), as the document at `url`, in place of the view's document.
void prPageViewLoad(PrPageView* view, PrStringView url, PrStringView contentType, PrByteView bytes);

// Declares the view in a round under `parent`, which it fills, runs the document's due work, and routes the round's
// input to it. Asks for frames while the document loads or fetches.
void prPageViewFrame(PrPageView* view, PrLayoutNode parent);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
