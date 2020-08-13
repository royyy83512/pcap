/*
 * Copyright (c) 2018 Tomasz Moń <desowin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */


#ifndef USBPCAPCMD_VERSION_H
#define USBPCAPCMD_VERSION_H

#define USBPCAPCMD_VERSION_MAJOR    1
#define USBPCAPCMD_VERSION_MINOR    5
#define USBPCAPCMD_VERSION_REVISION 4
#define USBPCAPCMD_VERSION_BUILD    0

#define makeStr(x) #x
#define makeString(x) makeStr(x)

#define USBPCAPCMD_VERSION_STR \
    makeString(USBPCAPCMD_VERSION_MAJOR) "." \
    makeString(USBPCAPCMD_VERSION_MINOR) "." \
    makeString(USBPCAPCMD_VERSION_REVISION) "." \
    makeString(USBPCAPCMD_VERSION_BUILD)

#endif /* USBPCAPCMD_VERSION_H */
