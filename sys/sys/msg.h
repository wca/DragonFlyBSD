/* $FreeBSD: src/sys/sys/msg.h,v 1.10.2.1 2000/08/04 22:31:10 peter Exp $ */
/*	$NetBSD: msg.h,v 1.4 1994/06/29 06:44:43 cgd Exp $	*/

/*
 * SVID compatible msg.h file
 *
 * Author:  Daniel Boulet
 *
 * Copyright 1993 Daniel Boulet and RTMX Inc.
 *
 * This system call was implemented by Daniel Boulet under contract from RTMX.
 *
 * Redistribution and use in source forms, with and without modification,
 * are permitted provided that this entire comment appears intact.
 *
 * Redistribution in binary form may occur without any restrictions.
 * Obviously, it would be nice if you gave credit where credit is due
 * but requiring it would be too onerous.
 *
 * This software is provided ``AS IS'' without any warranties of any kind.
 */

#ifndef _SYS_MSG_H_
#define _SYS_MSG_H_

#include <sys/ipc.h>

/*
 * The MSG_NOERROR identifier value, the msqid_ds struct and the msg struct
 * are as defined by the SV API Intel 386 Processor Supplement.
 */

#define MSG_NOERROR	010000		/* don't complain about too long msgs */

/*!!! In the kernel implementation, both msg_first and msg_last
 * have 'struct msg*' type.
 * In the userland implementation, a pointer to a msg is useless
 * because each message queue is mapped at different addresses in
 * the process space address so my choice was to use indexes.
 */
struct msg;

struct msqid_ds {
	struct	ipc_perm msg_perm;	/* msg queue permission bits */
	struct	msg *msg_first;	/* first message in the queue. */
	struct	msg *msg_last;	/* last message in the queue. */
	u_long	msg_cbytes;	/* number of bytes in use on the queue */
	u_long	msg_qnum;	/* number of msgs in the queue */
	u_long	msg_qbytes;	/* max # of bytes on the queue */
	pid_t	msg_lspid;	/* pid of last msgsnd() */
	pid_t	msg_lrpid;	/* pid of last msgrcv() */
	time_t	msg_stime;	/* time of last msgsnd() */
	long	msg_pad1;
	time_t	msg_rtime;	/* time of last msgrcv() */
	long	msg_pad2;
	time_t	msg_ctime;	/* time of last msgctl() */
	long	msg_pad3;
	long	msg_pad4[4];
};

/*
 * Structure describing a message.  The SVID doesn't suggest any
 * particular name for this structure.  There is a reference in the
 * msgop man page that reads "The structure mymsg is an example of what
 * this user defined buffer might look like, and includes the following
 * members:".  This sentence is followed by two lines equivalent
 * to the mtype and mtext field declarations below.  It isn't clear
 * if "mymsg" refers to the naem of the structure type or the name of an
 * instance of the structure...
 */
struct mymsg {
	long	mtype;		/* message type (+ve integer) */
	char	mtext[1];	/* message body */
};

#if defined(_KERNEL) || defined(_KERNEL_STRUCTURES)

/*
 * Based on the configuration parameters described in an SVR2 (yes, two)
 * config(1m) man page.
 *
 * Each message is broken up and stored in segments that are msgssz bytes
 * long.  For efficiency reasons, this should be a power of two.  Also,
 * it doesn't make sense if it is less than 8 or greater than about 256.
 * Consequently, msginit in kern/sysv_msg.c checks that msgssz is a power of
 * two between 8 and 1024 inclusive (and panic's if it isn't).
 */
struct msginfo {
	int	msgmax,		/* max chars in a message */
		msgmni,		/* max message queue identifiers */
		msgmnb,		/* max chars in a queue */
		msgtql,		/* max messages in system */
		msgssz,		/* size of a message segment (see notes above) */
		msgseg;		/* number of message segments */
};
#endif

#ifdef _KERNEL
extern struct msginfo	msginfo;
#endif

#ifndef _KERNEL

#include <sys/cdefs.h>

__BEGIN_DECLS
int msgctl (int, int, struct msqid_ds *);
int msgget (key_t, int);
int msgsnd (int, const void *, size_t, int);
int msgrcv (int, void *, size_t, long, int); /* XXX should return ssize_t */
__END_DECLS
#endif

#endif /* !_SYS_MSG_H_ */
