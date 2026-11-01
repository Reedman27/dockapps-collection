# Copyright 2026 Reedman27
# Distributed under the terms of the MIT License

EAPI=8

inherit git-r3 toolchain-funcs

DESCRIPTION="Media-control dockapp for Window Maker (playerctl/MPRIS or mpc/MPD)"
HOMEPAGE="https://github.com/Reedman27/dockapps-collection"
EGIT_REPO_URI="https://github.com/Reedman27/dockapps-collection.git"

# The repo holds several dockapps; check out the whole thing, build one.
EGIT_CHECKOUT_DIR="${WORKDIR}/${P}"
S="${WORKDIR}/${P}/wmmedia"

LICENSE="MIT"
SLOT="0"
# Live ebuild: no KEYWORDS on purpose.

IUSE="mpd"

# Upstream tests need Xvfb and fake players; not run during emerge.
RESTRICT="test"

DEPEND="
	x11-libs/libX11
	x11-libs/libXext
"
RDEPEND="
	${DEPEND}
	media-sound/playerctl
	mpd? ( media-sound/mpc )
"
BDEPEND="virtual/pkgconfig"

DOCS=( README.md CHANGELOG.md )

src_compile() {
	# The Makefile uses ?= for CC/CFLAGS and appends LDFLAGS, so
	# portage's environment is honoured as-is.
	tc-export CC PKG_CONFIG
	emake
}

src_install() {
	emake DESTDIR="${D}" PREFIX="${EPREFIX}/usr" install
	einstalldocs
}
