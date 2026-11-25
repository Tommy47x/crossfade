# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit cmake

DESCRIPTION="Native YouTube Music desktop player with real crossfade"
HOMEPAGE="https://github.com/XBoom31/crossfade"
SRC_URI="https://github.com/XBoom31/crossfade/archive/refs/tags/v${PV}.tar.gz -> ${P}.tar.gz"

LICENSE="GPL-3"
SLOT="0"
KEYWORDS="~amd64"
IUSE="webkit"

# crossfade falls back to the system Python when no bundled venv exists, so
# the ebuild intentionally does not create one: ytmusicapi and yt-dlp come
# from the tree and are imported/executed from system site-packages.
RDEPEND="
	dev-lang/python
	dev-libs/glib:2
	dev-libs/openssl:=
	dev-python/ytmusicapi
	media-libs/libsdl2
	media-video/ffmpeg
	media-video/mpv[libmpv]
	net-misc/curl
	net-misc/yt-dlp
	virtual/opengl
	x11-libs/libnotify
	webkit? (
		|| (
			net-libs/webkit-gtk:4.1
			net-libs/webkit-gtk:6.0
		)
	)
"
DEPEND="${RDEPEND}"
BDEPEND="virtual/pkgconfig"

src_configure() {
	# Unit tests exercise the dev tree and are not part of the installed app.
	local mycmakeargs=(
		-DCROSSFADE_BUILD_TESTS=OFF
	)
	cmake_src_configure
}
