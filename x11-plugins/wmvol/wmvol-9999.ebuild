# Copyright 2026 Reedman27
# Distributed under the terms of the MIT License

EAPI=8

inherit git-r3 toolchain-funcs

DESCRIPTION="PipeWire volume dockapp for Window Maker with a popup mixer"
HOMEPAGE="https://github.com/Reedman27/dockapps-collection"
EGIT_REPO_URI="https://github.com/Reedman27/dockapps-collection.git"

# The repo holds several dockapps; check out the whole thing, build one.
EGIT_CHECKOUT_DIR="${WORKDIR}/${P}"
S="${WORKDIR}/${P}/wmvol"

LICENSE="MIT"
SLOT="0"
# Live ebuild: no KEYWORDS on purpose.

# Upstream tests drive a fake wpctl under Xvfb; not run during emerge.
RESTRICT="test"

DEPEND="
	x11-libs/libX11
	x11-libs/libXext
	x11-libs/libXft
"
# wpctl comes from wireplumber; pactl (libpulse) is optional, wmvol polls without it.
RDEPEND="
	${DEPEND}
	media-video/wireplumber
	media-libs/libpulse
"
BDEPEND="virtual/pkgconfig"

DOCS=( README.md CHANGELOG.md )

src_compile() {
	# The Makefile uses ?= and += for CC/CFLAGS/LDFLAGS, so the
	# environment (portage's CFLAGS/LDFLAGS) is honoured as-is.
	tc-export CC
	emake
}

src_install() {
	emake DESTDIR="${D}" PREFIX="${EPREFIX}/usr" install
	einstalldocs
}
