#ifndef __NFS4ACL_INTERNAL_H
#define __NFS4ACL_INTERNAL_H

/*
 * The ACE4_READ_ATTRIBUTES and ACE4_READ_ACL flags are always granted
 * in POSIX. The ACE4_SYNCHRONIZE flag has no meaning under POSIX.
 */
#define ACE4_POSIX_ALWAYS_ALLOWED ( \
	ACE4_SYNCHRONIZE | \
	ACE4_READ_ATTRIBUTES | \
	ACE4_READ_ACL )

/* e_flags bitflags */
#define ACE4_SPECIAL_WHO		0x4000  /* internal to the library */

static inline void
nfs4ace_clear_inheritance_flags(struct nfs4ace *ace)
{
	ace->e_flags &= ~(ACE4_FILE_INHERIT_ACE |
			  ACE4_DIRECTORY_INHERIT_ACE |
			  ACE4_NO_PROPAGATE_INHERIT_ACE |
			  ACE4_INHERIT_ONLY_ACE);
}

static inline int nfs4ace_is_inheritable(const struct nfs4ace *ace)
{
	return ace->e_flags & (ACE4_FILE_INHERIT_ACE |
			       ACE4_DIRECTORY_INHERIT_ACE);
}

static inline int nfs4ace_is_inherit_only(const struct nfs4ace *ace)
{
	return ace->e_flags & ACE4_INHERIT_ONLY_ACE;
}

extern const char *nfs4ace_owner_who;
extern const char *nfs4ace_group_who;
extern const char *nfs4ace_everyone_who;

#endif  /* __NFS4ACL_INTERNAL_H */
