#ifndef __UAPI_DIM_SUM_FS_H
#define __UAPI_DIM_SUM_FS_H

enum {
	/**
	 * 内部 伪文件系统，如块设备文件系统
	 * 不能被mount到用户空间
	 */
	__MFLAG_INTERNAL,
	/**
	 * 只读mount
	 */
	__MFLAG_RDONLY,
	/**
	 * 禁止setuid和setgid标志。
	 */
	__MS_NOSUID,
	/**
	 * 禁止访问设备文件。
	 */
	__MS_NODEV,
	/**
	 * 不允许文件执行。
	 */
	__MS_NOEXEC,
	/**
	 * 文件和目录上的写操作是即时的。在mount时指定了sync参数。
	 */
	__MS_SYNCHRONOUS,
	/**
	 * 重新安装文件系统。
	 */
	__MS_REMOUNT,
	/**
	 * 允许强制加锁。
	 */
	__MS_MANDLOCK,
	/**
	 * 目录上的写操作是即时的。
	 */
	__MS_DIRSYNC,
	/**
	 * 不更新文件访问时间。
	 */
	__MS_NOATIME,
	/**
	 * 不更新目录访问时间。
	 */
	__MS_NODIRATIME,
	/**
	 * 创建一个"绑定安装"。这样一个文件或目录在系统的另外一个点上可以被看见。参见mount命令的__bind选项。
	 */
	__MS_BIND,
	/**
	 * 自动把一个已安装文件系统移动到另外一个安装点。参见mount命令的__move选项。
	 */
	__MS_MOVE,
	/**
	 * 为目录子树递归地创建"绑定安装"
	 */
	__MS_REC,
	/**
	 * 出错时产生详细的内核消息。
	 */
	__MS_VERBOSE,
	__MS_POSIXACL,
	/**
	 * 内部使用
	 */
	__MS_ACTIVE = 30,
};

/**
 * MOUNT标志
 */
#define MFLAG_INTERNAL	(1UL << __MFLAG_INTERNAL)
#define MFLAG_RDONLY	(1UL << __MFLAG_RDONLY)
#define MS_NOSUID	(1UL << __MS_NOSUID)
#define MS_NODEV	(1UL << __MS_NODEV)
#define MS_NOEXEC	(1UL << __MS_NOEXEC)
#define MS_SYNCHRONOUS	(1UL << __MS_SYNCHRONOUS)
#define MS_REMOUNT	(1UL << __MS_REMOUNT)
#define MS_MANDLOCK	(1UL << __MS_MANDLOCK)
#define MS_DIRSYNC	(1UL << __MS_DIRSYNC)
#define MS_NOATIME	(1UL << __MS_NOATIME)
#define MS_NODIRATIME	(1UL << __MS_NODIRATIME)
#define MS_BIND		(1UL << __MS_BIND)
#define MS_MOVE		(1UL << __MS_MOVE)
#define MS_REC		(1UL << __MS_REC)
#define MS_VERBOSE	(1UL << __MS_VERBOSE)
#define MS_POSIXACL	(1UL << __MS_POSIXACL)
#define MS_ACTIVE	(1UL << __MS_ACTIVE)

#define SEEK_SET	0	/* seek relative to beginning of file */
#define SEEK_CUR	1	/* seek relative to current file position */
#define SEEK_END	2	/* seek relative to end of file */

#endif /* __UAPI_DIM_SUM_FS_H */
