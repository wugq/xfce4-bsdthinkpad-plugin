PORTNAME=	tposd
DISTVERSION=	0.1.1
CATEGORIES=	sysutils xfce
# No download site yet: put the tarball from "make dist" (top directory of
# the tposd sources) into ${DISTDIR}, then "make makesum".  Once the sources
# are on GitHub, replace this with
#	USE_GITHUB=	yes
#	GH_ACCOUNT=	<account>
MASTER_SITES=	# none

MAINTAINER=	wugq.dev@gmail.com
COMMENT=	On-screen display and XFCE panel plugin for ThinkPad keys

LICENSE=	BSD2CLAUSE
LICENSE_FILE=	${WRKSRC}/LICENSE

ONLY_FOR_ARCHS=	amd64 i386
ONLY_FOR_ARCHS_REASON=	ThinkPad hardware (acpi_ibm, acpi_call)

LIB_DEPENDS=	libnotify.so:devel/libnotify
RUN_DEPENDS=	${LOCALBASE}/sbin/acpi_call:sysutils/acpi_call \
		pkexec:sysutils/polkit \
		ck-list-sessions:sysutils/consolekit2

USES=		gnome pkgconfig xfce
USE_GNOME=	glib20 gtk30
USE_XFCE=	panel

MAKE_ARGS=	PREFIX=${PREFIX} LOCALBASE=${LOCALBASE}
SUB_FILES=	pkg-message

post-install:
	${STRIP_CMD} ${STAGEDIR}${PREFIX}/bin/tposd \
		${STAGEDIR}${PREFIX}/libexec/tposd-mute-led \
		${STAGEDIR}${PREFIX}/lib/xfce4/panel/plugins/libtposd-panel.so

.include <bsd.port.mk>
