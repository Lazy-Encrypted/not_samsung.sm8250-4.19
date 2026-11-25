/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TACHYON_H
#define _LINUX_TACHYON_H

#define TACHYON_NAME		"Tachyon Kernel"
#define TACHYON_VERSION		"Tachyon-r8q"
#define TACHYON_CODENAME	"Beyond the Speed of Light"
#define TACHYON_AUTHOR		"noxy"
#define TACHYON_MAINTAINER	"noxy <noxy@debian.noxy>"
#define TACHYON_CHANNEL		"@noxydevelopment"
#define TACHYON_TARGET		"Samsung Galaxy S20 FE 5G (SM8250 / r8q)"
#define TACHYON_MAGIC		0x54414348 /* 'TACH' */

/* Core author identity and anti-tamper interface */
extern void tachyon_banner(void);
extern int tachyon_validate_integrity(void);

#endif /* _LINUX_TACHYON_H */
