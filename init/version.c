/*
 *  linux/init/version.c
 *
 *  Copyright (C) 1992  Theodore Ts'o
 *
 *  May be freely distributed as part of Linux.
 */

#include <linux/compile.h>
#include <linux/module.h>
#include <linux/uts.h>
#include <linux/utsname.h>
#include <linux/version.h>

#define version(a) Version_ ## a
#define version_string(a) version(a)

int version_string(LINUX_VERSION_CODE);
// uname -a ��ʾ����Ϣ
struct new_utsname system_utsname = {
	.sysname	= UTS_SYSNAME,	//ϵͳ���ƣ��̶�Ϊ linux
	.nodename	= UTS_NODENAME, //���������� sethostname ����
	.release	= UTS_RELEASE,  //���а汾��
	.version	= UTS_VERSION,  //��������
	.machine	= UTS_MACHINE,  // ��������
	.domainname	= UTS_DOMAINNAME, // ������ �� setdomainname ����
};

EXPORT_SYMBOL(system_utsname);

const char linux_banner[] =
	"Linux version " UTS_RELEASE " (" LINUX_COMPILE_BY "@"
	LINUX_COMPILE_HOST ") (" LINUX_COMPILER ") " UTS_VERSION "\n";
