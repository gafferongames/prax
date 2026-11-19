"""The conformance suites: what each reads, the runner that drives it, and the harnesses that the runner needs.

`./dev.sh suite <name>` fetches what a suite reads, builds, and runs it. A suite's data, its errata and its scoreboard,
is under suites/, in the directory that its runner names.
"""

import os
import sys

from tools import dev


class Suite:
    """A suite. `checkouts` are what it reads (tools/fetch/checkouts.py), and `corpus` the paths under third_party whose
    presence says that it is checked out. `runner` is the module that runs it and the arguments before the run's own,
    and `harnesses` the build's targets that the runner drives. `unsupported` says why the suite cannot run on this
    platform, or is None if it can."""

    def __init__(self, name, checkouts, corpus, runner, harnesses, unsupported=None):
        self.name = name
        self.checkouts = checkouts
        self.corpus = corpus
        self.runner = runner
        self.harnesses = harnesses
        self.unsupported = unsupported

    def ready(self, directory):
        """Returns true if the suite's corpus is checked out and its harnesses are built in `directory`."""
        return (all(os.path.exists(os.path.join(dev.ROOT, "third_party", path)) for path in self.corpus)
                and all(os.path.exists(dev.executable(directory, harness)) for harness in self.harnesses))


def reftest(name, checkouts, corpus, harnesses=("reftest",)):
    """A suite whose references are images (tools/suite/reftest.py)."""
    return Suite(name, checkouts, corpus, ("tools.suite.reftest", name), harnesses)


def wpt(name):
    """A suite of web-platform-tests, run in pr_shell (tools/suite/wpt/wpt.py). The runner renders the SVG documents of
    reftests with the reftest harness."""
    return Suite(name + "-wpt", ("wpt", "webfeatures", "fonts"), ("wpt/.git",), ("tools.suite.wpt.wpt", name),
                 ("pr_shell", "reftest"))


SUITES = [
    Suite("css-parsing", ("cssparser",), ("cssparser/src",), ("tools.suite.css_parsing",), ("css_parsing",)),
    *(wpt(name) for name in ("css", "dom", "encoding", "html", "infrastructure", "networking")),
    Suite("html-tokens", ("html5lib",), ("html5lib/tokenizer",), ("tools.suite.html_tokens",), ("html_tokens",)),
    Suite("html-tree", ("wpt",), ("wpt/.git",), ("tools.suite.html_tree",), ("html_tree",)),
    Suite("url-parsing", ("wpt",), ("wpt/.git",), ("tools.suite.url_parsing",), ("url_parsing",)),
    # The conformance files of the Unicode Character Database and of UTS #46, against lib/unicode.
    Suite("unicode", ("unicode",), ("unicode/BidiTest.txt",), ("tools.suite.unicode",), ("unicode_conformance",)),
    *(wpt(name) for name in ("url", "mathml", "media-source", "performance", "svg", "webaudio", "xhr")),
    # The WPT runner's own tests against the checkout and its server, and the rulings of web_features.txt.
    Suite("wpt-runner", ("wpt", "webfeatures"), ("wpt/.git",), ("tools.suite.wpt.runner_tests",),
          ("pr_shell", "reftest")),
    # The font bundles' coverage of each common language's exemplar characters, as CLDR gives them.
    Suite("font-coverage", ("cldr",), ("cldr/common/main",), ("tools.suite.font_coverage",), ()),
    reftest("resvg", ("resvg_tests", "fonts"), ("resvg-tests/tests",)),
    reftest("lottie-web", ("lottie_corpora", "lottie_web"), ("lottie-web/version",), ("reftest", "lottie_reference")),
    reftest("skottie", ("lottie_corpora", "canvaskit", "resvg_tests"), ("canvaskit/version",),
            ("reftest", "lottie_reference")),
    reftest("rlottie", ("lottie_corpora",), ("lottie/rlottie",), ("reftest", "rlottie_reference")),
    reftest("lottie-ios", ("lottie_corpora",), ("lottie/lottie-ios/Tests/__Snapshots__",)),
    Suite("commonmark", ("commonmark",), ("commonmark-spec/spec.txt",),
          ("tools.suite.markdown.markdown_spec", "commonmark"), ("markdown_spec",)),
    Suite("gfm", ("cmark_gfm",), ("cmark-gfm/test/spec.txt",), ("tools.suite.markdown.markdown_spec", "gfm"),
          ("markdown_spec",)),
    Suite("markdown-dom", ("commonmark", "cmark_gfm"), ("commonmark-spec/spec.txt", "cmark-gfm/test/spec.txt"),
          ("tools.suite.markdown.markdown_dom",), ("markdown_dom",)),
    # Khronos's glTF sample assets, read by lib/gltf.
    Suite("gltf", ("gltf_sample_assets",), ("glTF-Sample-Assets/Models",), ("tools.suite.gltf",), ("gltf_corpus",)),
    # The same assets, loaded by lib/model and rendered by lib/render.
    Suite("model", ("gltf_sample_assets",), ("glTF-Sample-Assets/Models",), ("tools.suite.model",),
          ("model_corpus", "gltf_corpus")),
    # The same assets' tangents, from lib/mesh beside MikkTSpace's reference implementation and meshoptimizer.
    Suite("tangents", ("gltf_sample_assets",), ("glTF-Sample-Assets/Models",), ("tools.suite.tangents",),
          ("tangents_compare", "gltf_corpus")),
    # libwebp's test data and the WebP images of the same assets, decoded by lib/webp beside libwebp.
    Suite("webp", ("libwebp_test_data", "gltf_sample_assets"), ("libwebp-test-data/.git", "glTF-Sample-Assets/Models"),
          ("tools.suite.webp",), ("webp_compare",)),
    # OpenEXR's test images, the StandardShaderBall's textures, and images that OpenEXR's encoder writes in every
    # compression, decoded by lib/exr beside OpenEXR's core library.
    Suite("exr", ("openexr_images", "usd_assets"), ("openexr-images/.git", "usd-assets/full_assets"),
          ("tools.suite.exr",), ("exr_compare",)),
    # The Draco streams of the same assets, decoded by lib/draco beside the reference decoder.
    Suite("draco", ("gltf_sample_assets",), ("glTF-Sample-Assets/Models",), ("tools.suite.draco",), ("draco_compare",)),
    # The USD working group's assets and the core specification's supplement, read by lib/usd and compared with
    # Pixar's OpenUSD, which runs from its wheel in a Python environment of its own.
    Suite("usd", ("usd_assets", "usd_spec_supplemental", "usd_core"), ("usd-assets/full_assets",), ("tools.suite.usd",),
          ("usd_corpus",)),
    # Every file of the corpora above, classified by lib/mime.
    Suite("classify", ("gltf_sample_assets", "usd_assets", "libwebp_test_data"),
          ("glTF-Sample-Assets/Models", "usd-assets/full_assets", "libwebp-test-data/.git"), ("tools.suite.classify",),
          ("mime_corpus",)),
    # esctest needs termios, and a Windows pseudoconsole would rewrite the sequences that it tests.
    Suite("esctest", ("esctest",), ("esctest2/esctest",), ("tools.suite.esctest",), ("vterm_esctest",),
          "esctest runs on a POSIX pseudoterminal" if sys.platform == "win32" else None),
]

BY_NAME = {suite.name: suite for suite in SUITES}


def named(name):
    """Returns the suite that `name` names, or exits naming every suite."""
    if name not in BY_NAME:
        dev.fail("no suite named %s; one of: %s" % (name, " ".join(BY_NAME)))
    return BY_NAME[name]
