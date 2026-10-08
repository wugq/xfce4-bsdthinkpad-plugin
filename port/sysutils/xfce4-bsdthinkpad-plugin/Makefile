PORTNAME=	xfce4-bsdthinkpad-plugin
DISTVERSIONPREFIX=	v
DISTVERSION=	0.2.2
CATEGORIES=	sysutils xfce

MAINTAINER=	wugq.dev@gmail.com
COMMENT=	XFCE panel plugin and OSD for ThinkPad keys on FreeBSD
WWW=		https://github.com/wugq/xfce4-bsdthinkpad-plugin

LICENSE=	BSD2CLAUSE
LICENSE_FILE=	${WRKSRC}/LICENSE

ONLY_FOR_ARCHS=	amd64 i386
ONLY_FOR_ARCHS_REASON=	ThinkPad hardware (acpi_ibm, acpi_call)

LIB_DEPENDS=	libnotify.so:devel/libnotify
RUN_DEPENDS=	${LOCALBASE}/sbin/acpi_call:sysutils/acpi_call \
		pkexec:sysutils/polkit \
		ck-list-sessions:sysutils/consolekit2

USES=		gnome pkgconfig xfce
USE_GITHUB=	yes
GH_ACCOUNT=	wugq
USE_GNOME=	glib20 gtk30
USE_XFCE=	panel

MAKE_ARGS=	PREFIX=${PREFIX} LOCALBASE=${LOCALBASE}
SUB_FILES=	pkg-message

post-install:
	${STRIP_CMD} ${STAGEDIR}${PREFIX}/bin/bsdthinkpad \
		${STAGEDIR}${PREFIX}/libexec/bsdthinkpad-mute-led \
		${STAGEDIR}${PREFIX}/lib/xfce4/panel/plugins/libbsdthinkpad-plugin.so

.include <bsd.port.mk>
