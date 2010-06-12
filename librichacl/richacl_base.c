/*
  Copyright (C) 2006, 2009, 2010  Novell, Inc.
  Written by Andreas Gruenbacher <agruen@suse.de>

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <alloca.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include "string_buffer.h"
#include "richacl.h"
#include "richacl-internal.h"

const char *richace_owner_who	 = "OWNER@";
const char *richace_group_who	 = "GROUP@";
const char *richace_everyone_who = "EVERYONE@";

static struct {
	char		a_char;
	unsigned char	a_flag;
	const char	*a_name;
} acl_flag_bits[] = {
	{ 'a', ACL4_AUTO_INHERIT, "auto_inherit" },
	{ 'p', ACL4_PROTECTED, "protected" },
	{ 'd', ACL4_DEFAULTED, "defaulted" },
	{ 'P', ACL4_POSIX_MAPPED, "posix_mapped"},
};

static struct {
	uint16_t	e_type;
	const char	*e_name;
} type_values[] = {
	{ ACE4_ACCESS_ALLOWED_ACE_TYPE, "allow" },
	{ ACE4_ACCESS_DENIED_ACE_TYPE,  "deny" },
};

#define FLAGS_BIT(c, name, str) \
	{ ACE4_ ## name, c, #name }

static struct {
	uint16_t	e_flag;
	char		e_char;
	const char	*e_name;
} ace_flag_bits[] = {
	FLAGS_BIT('f', FILE_INHERIT_ACE, "file_inherit_ace"),
	FLAGS_BIT('d', DIRECTORY_INHERIT_ACE, "directory_inherit_ace"),
	FLAGS_BIT('n', NO_PROPAGATE_INHERIT_ACE, "no_propagate_inherit_ace"),
	FLAGS_BIT('i', INHERIT_ONLY_ACE, "inherit_only_ace"),
	FLAGS_BIT('g', IDENTIFIER_GROUP, "identifier_group"),
	FLAGS_BIT('a', INHERITED_ACE, "inherited_ace"),
};

#undef FLAGS_BIT

#define MASK_BIT(c, name, str) \
	{ ACE4_ ## name, c, str, RICHACL_TEXT_FILE_CONTEXT | \
				 RICHACL_TEXT_DIRECTORY_CONTEXT }
#define FILE_MASK_BIT(c, name, str) \
	{ ACE4_ ## name, c, str, RICHACL_TEXT_FILE_CONTEXT }
#define DIRECTORY_MASK_BIT(c, name, str) \
	{ ACE4_ ## name, c, str, RICHACL_TEXT_DIRECTORY_CONTEXT }

struct mask_flag_struct {
	uint32_t	e_mask;
	char		e_char;
	const char	*e_name;
	int		e_context;
};
struct mask_flag_struct mask_flags[] = {
	MASK_BIT('*', VALID_MASK, "*"),
	FILE_MASK_BIT('r', READ_DATA, "read_data"),
	DIRECTORY_MASK_BIT('r', LIST_DIRECTORY, "list_directory"),
	FILE_MASK_BIT('w', WRITE_DATA, "write_data"),
	DIRECTORY_MASK_BIT('w', ADD_FILE, "add_file"),
	FILE_MASK_BIT('a', APPEND_DATA, "append_data"),
	DIRECTORY_MASK_BIT('a', ADD_SUBDIRECTORY, "add_subdirectory"),
	MASK_BIT('x', EXECUTE, "execute"),
	/* DELETE_CHILD is only meaningful for directories but it might also
	   be set in an ACE of a file, so print it in file context as well.  */
	MASK_BIT('d', DELETE_CHILD, "delete_child"),
	MASK_BIT('T', READ_ATTRIBUTES, "read_attributes"),
	MASK_BIT('t', WRITE_ATTRIBUTES, "write_attributes"),
	MASK_BIT('D', DELETE, "delete"),
	MASK_BIT('M', READ_ACL, "read_acl"),
	MASK_BIT('m', WRITE_ACL, "write_acl"),
	MASK_BIT('o', WRITE_OWNER, "take_ownership"),
	MASK_BIT('s', SYNCHRONIZE, "synchronize"),
	MASK_BIT('N', READ_NAMED_ATTRS, "read_named_attrs"),
	MASK_BIT('n', WRITE_NAMED_ATTRS, "write_named_attrs"),
	MASK_BIT('e', WRITE_RETENTION, "write_retention"),
	MASK_BIT('E', WRITE_RETENTION_HOLD, "write_retention_hold"),
};
struct mask_flag_struct mask_bits[] = {
	MASK_BIT('r', READ_DATA, NULL),
	MASK_BIT('w', WRITE_DATA, NULL),
	MASK_BIT('a', APPEND_DATA, NULL),
	MASK_BIT('x', EXECUTE, "execute"),
	MASK_BIT('d', DELETE_CHILD, "delete_child"),
	MASK_BIT('T', READ_ATTRIBUTES, "read_attributes"),
	MASK_BIT('t', WRITE_ATTRIBUTES, "write_attributes"),
	MASK_BIT('D', DELETE, "delete"),
	MASK_BIT('M', READ_ACL, "read_acl"),
	MASK_BIT('m', WRITE_ACL, "write_acl"),
	MASK_BIT('o', WRITE_OWNER, "take_ownership"),
	MASK_BIT('s', SYNCHRONIZE, "synchronize"),
	MASK_BIT('N', READ_NAMED_ATTRS, "read_named_attrs"),
	MASK_BIT('n', WRITE_NAMED_ATTRS, "write_named_attrs"),
	MASK_BIT('e', WRITE_RETENTION, "write_retention"),
	MASK_BIT('E', WRITE_RETENTION_HOLD, "write_retention_hold"),
};

#undef MASK_BIT
#undef FILE_MASK_BIT
#undef DIRECTORY_MASK_BIT

const char *richace_get_who(const struct richace *ace)
{
	if (!(ace->e_flags & ACE4_SPECIAL_WHO))
		return NULL;
	return ace->u.e_who;
}

int richace_is_same_identifier(const struct richace *a, const struct richace *b)
{
#define WHO_FLAGS (ACE4_SPECIAL_WHO | ACE4_IDENTIFIER_GROUP)
	if ((a->e_flags & WHO_FLAGS) != (b->e_flags & WHO_FLAGS))
		return 0;
	if (a->e_flags & ACE4_SPECIAL_WHO)
		return a->u.e_who == b->u.e_who;
	else
		return a->u.e_id == b->u.e_id;
#undef WHO_FLAGS
}

int richace_is_owner(const struct richace *ace)
{
	return (ace->e_flags & ACE4_SPECIAL_WHO) &&
		ace->u.e_who == richace_owner_who;
}

int richace_is_group(const struct richace *ace)
{
	return (ace->e_flags & ACE4_SPECIAL_WHO) &&
		ace->u.e_who == richace_group_who;
}

int richace_is_everyone(const struct richace *ace)
{
	return (ace->e_flags & ACE4_SPECIAL_WHO) &&
		ace->u.e_who == richace_everyone_who;
}

struct richacl *richacl_alloc(size_t count)
{
	size_t size = sizeof(struct richacl) + count * sizeof(struct richace);
	struct richacl *acl = malloc(size);

	if (acl) {
		memset(acl, 0, size);
		acl->a_count = count;
	}
	return acl;
}

struct richacl *richacl_clone(struct richacl *acl)
{
	size_t size;
	struct richacl *acl2;

	if (!acl)
		return NULL;
	size = sizeof(struct richacl) + acl->a_count * sizeof(struct richace);
	acl2 = malloc(size);
	if (acl2)
		memcpy(acl2, acl, size);
	return acl2;
}

void richacl_free(struct richacl *acl)
{
	free(acl);
}

/**
 * richacl_allowed_to_who  -  mask flags allowed to a specific who value
 *
 * Computes the mask values allowed to a specific who value, taking
 * EVERYONE@ entries into account.
 */
static unsigned int richacl_allowed_to_who(struct richacl *acl,
					   struct richace *who)
{
	struct richace *ace;
	unsigned int allowed = 0;

	richacl_for_each_entry_reverse(ace, acl) {
		if (richace_is_inherit_only(ace))
			continue;
		if (richace_is_same_identifier(ace, who) ||
		    richace_is_everyone(ace)) {
			if (richace_is_allow(ace))
				allowed |= ace->e_mask;
			else if (richace_is_deny(ace))
				allowed &= ~ace->e_mask;
		}
	}
	return allowed;
}

/**
 * richacl_max_masks  -  compute upper bound masks
 *
 * Computes upper bound owner, group, and other masks so that none of
 * the mask flags allowed by the acl are disabled (for any choice of the
 * file owner or group membership).
 */
void richacl_max_masks(struct richacl *acl)
{
	struct richace *ace;
	int had_group_ace = 0;

	acl->a_owner_mask = 0;
	acl->a_group_mask = 0;
	acl->a_other_mask = 0;

	richacl_for_each_entry_reverse(ace, acl) {
		if (richace_is_inherit_only(ace))
			continue;

		if (richace_is_owner(ace)) {
			if (richace_is_allow(ace))
				acl->a_owner_mask |= ace->e_mask;
			else if (richace_is_deny(ace))
				acl->a_owner_mask &= ~ace->e_mask;
		} else if (richace_is_everyone(ace)) {
			if (richace_is_allow(ace)) {
				acl->a_owner_mask |= ace->e_mask;
				acl->a_other_mask |= ace->e_mask;
			} else if (richace_is_deny(ace)) {
				acl->a_owner_mask &= ~ace->e_mask;
				acl->a_other_mask &= ~ace->e_mask;
			}
		} else {
			unsigned int mask =
				richacl_allowed_to_who(acl, ace);

			acl->a_group_mask |= mask;
			acl->a_owner_mask |= mask;

			if (richace_is_group(ace))
				had_group_ace = 1;
		}
	}

	if (!had_group_ace)
		acl->a_group_mask |= acl->a_other_mask;
}

static void write_acl_flags(struct string_buffer *buffer, unsigned char flags, int fmt)
{
	int cont = 0, i;

	if (!flags)
		return;
	buffer_sprintf(buffer, "flags:");
	for (i = 0; i < ARRAY_SIZE(acl_flag_bits); i++) {
		if (!(flags & acl_flag_bits[i].a_flag))
			continue;

		flags &= ~acl_flag_bits[i].a_flag;
		if (fmt & RICHACL_TEXT_LONG) {
			if (cont)
				buffer_sprintf(buffer, "/");
			buffer_sprintf(buffer, "%s", acl_flag_bits[i].a_name);
		} else
			buffer_sprintf(buffer, "%c", acl_flag_bits[i].a_char);
		cont = 1;
	}
	if (flags) {
		if (cont)
			buffer_sprintf(buffer, "/");
		buffer_sprintf(buffer, "0x%x", flags);
	}
	buffer_sprintf(buffer, "\n");
}

static void write_type(struct string_buffer *buffer, uint16_t type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(type_values); i++) {
		if (type == type_values[i].e_type) {
			buffer_sprintf(buffer, "%s", type_values[i].e_name);
			break;
		}
	}
	if (i == ARRAY_SIZE(type_values))
		buffer_sprintf(buffer, "%u", type);
}

static void write_ace_flags(struct string_buffer *buffer, uint16_t flags, int fmt)
{
	int cont = 0, i;

	flags &= ~ACE4_SPECIAL_WHO;

	for (i = 0; i < ARRAY_SIZE(ace_flag_bits); i++) {
		if (!(flags & ace_flag_bits[i].e_flag))
			continue;

		flags &= ~ace_flag_bits[i].e_flag;
		if (fmt & RICHACL_TEXT_LONG) {
			if (cont)
				buffer_sprintf(buffer, "/");
			buffer_sprintf(buffer, "%s", ace_flag_bits[i].e_name);
		} else
			buffer_sprintf(buffer, "%c", ace_flag_bits[i].e_char);
		cont = 1;
	}
	if (flags) {
		if (cont)
			buffer_sprintf(buffer, "/");
		buffer_sprintf(buffer, "0x%x", flags);
	}
}

static void write_mask(struct string_buffer *buffer, uint32_t mask, int fmt)
{
	int stuff_written = 0, i;

	if (fmt & RICHACL_TEXT_LONG) {
		unsigned int nondir_mask, dir_mask;

		/*
		 * In long format, we write the non-directory and/or directory mask
		 * name depending on the context which applies. The short format
		 * does not distinguish between the two, so make sure that we won't
		 * repeat the same mask letters.
		 */
		if (!(fmt & (RICHACL_TEXT_FILE_CONTEXT |
			     RICHACL_TEXT_DIRECTORY_CONTEXT)))
			fmt |= RICHACL_TEXT_FILE_CONTEXT |
			       RICHACL_TEXT_DIRECTORY_CONTEXT;
		if (!(fmt & RICHACL_TEXT_LONG) &&
		    (fmt & RICHACL_TEXT_FILE_CONTEXT))
			fmt &= ~RICHACL_TEXT_DIRECTORY_CONTEXT;

		nondir_mask = (fmt & RICHACL_TEXT_FILE_CONTEXT) ? mask : 0;
		dir_mask = (fmt & RICHACL_TEXT_DIRECTORY_CONTEXT) ? mask : 0;

		for (i = 0; i < ARRAY_SIZE(mask_flags); i++) {
			int found = 0;

			if ((nondir_mask & mask_flags[i].e_mask) ==
			    mask_flags[i].e_mask &&
			    (mask_flags[i].e_context & RICHACL_TEXT_FILE_CONTEXT)) {
				nondir_mask &= ~mask_flags[i].e_mask;
				found = 1;
			}
			if ((dir_mask & mask_flags[i].e_mask) == mask_flags[i].e_mask &&
			    (mask_flags[i].e_context & RICHACL_TEXT_DIRECTORY_CONTEXT)) {
				dir_mask &= ~mask_flags[i].e_mask;
				found = 1;
			}
			if (found) {
				if (fmt & RICHACL_TEXT_SIMPLIFY) {
					/* Hide permissions that are always allowed. */
					if (mask_flags[i].e_mask ==
					    (mask_flags[i].e_mask &
					     ACE4_POSIX_ALWAYS_ALLOWED))
						continue;
				}
				if (fmt & RICHACL_TEXT_LONG) {
					if (stuff_written)
						buffer_sprintf(buffer, "/");
					buffer_sprintf(buffer, "%s",
						       mask_flags[i].e_name);
				} else
					buffer_sprintf(buffer, "%c",
						       mask_flags[i].e_char);
				stuff_written = 1;
			}
		}
		mask &= (nondir_mask | dir_mask);
	} else {
		unsigned int hide = 0;

		if (fmt & RICHACL_TEXT_SIMPLIFY) {
			hide = ACE4_POSIX_ALWAYS_ALLOWED;
#if 0
			/* Hiding directory-specific flags leads to misaligned,
			   so disable this. */

			/* ACE4_DELETE_CHILD is meaningless for non-directories. */
			if (!(fmt & RICHACL_TEXT_DIRECTORY_CONTEXT))
				hide |= ACE4_DELETE_CHILD;
#endif
		}

		for (i = 0; i < ARRAY_SIZE(mask_bits); i++) {
			if (!(mask_bits[i].e_mask & hide)) {
				if (mask & mask_bits[i].e_mask)
					buffer_sprintf(buffer, "%c",
						       mask_bits[i].e_char);
				else if (fmt & RICHACL_TEXT_ALIGN)
					buffer_sprintf(buffer, "-");
			}
			mask &= ~mask_bits[i].e_mask;
		}
		stuff_written = 1;
	}
	if (mask) {
		if (stuff_written)
			buffer_sprintf(buffer, "/");
		buffer_sprintf(buffer, "0x%x", mask);
	}
}

static void write_identifier(struct string_buffer *buffer,
			     const struct richace *ace, int align, int fmt)
{
	/* FIXME: switch to getpwuid_r() and getgrgid_r() here. */

	if (ace->e_flags & ACE4_SPECIAL_WHO) {
		char *dup, *c;

		dup = alloca(strlen(ace->u.e_who) + 1);
		strcpy(dup, ace->u.e_who);
		for (c = dup; *c; c++)
			*c = tolower(*c);

		buffer_sprintf(buffer, "%*s", align, dup);
	} else if (ace->e_flags & ACE4_IDENTIFIER_GROUP) {
		struct group *group = NULL;

		if (!(fmt & RICHACL_TEXT_NUMERIC_IDS))
			group = getgrgid(ace->u.e_id);
		if (group)
			buffer_sprintf(buffer, "%*s", align, group->gr_name);
		else
			buffer_sprintf(buffer, "%*d", align, ace->u.e_id);
	} else {
		struct passwd *passwd = NULL;

		if (!(fmt & RICHACL_TEXT_NUMERIC_IDS))
			passwd = getpwuid(ace->u.e_id);
		if (passwd)
			buffer_sprintf(buffer, "%*s", align, passwd->pw_name);
		else
			buffer_sprintf(buffer, "%*d", align, ace->u.e_id);
	}
}

char *richacl_to_text(const struct richacl *acl, int fmt)
{
	struct string_buffer *buffer;
	const struct richace *ace;
	int fmt2, align = 0;

	if (fmt & RICHACL_TEXT_ALIGN) {
		if (fmt & RICHACL_TEXT_SHOW_MASKS)
			align = 6;
		richacl_for_each_entry(ace, acl) {
			int a;
			if (richace_is_owner(ace) || richace_is_group(ace))
				a = 6;
			else if (richace_is_everyone(ace))
				a = 9;
			else if (ace->e_flags & ACE4_IDENTIFIER_GROUP) {
				struct group *group = NULL;

				if (!(fmt & RICHACL_TEXT_NUMERIC_IDS))
					group = getgrgid(ace->u.e_id);
				if (group)
					a = strlen(group->gr_name);
				else
					a = snprintf(NULL, 0, "%d", ace->u.e_id);
			} else {
				struct passwd *passwd = NULL;

				if (!(fmt & RICHACL_TEXT_NUMERIC_IDS))
					passwd = getpwuid(ace->u.e_id);
				if (passwd)
					a = strlen(passwd->pw_name);
				else
					a = snprintf(NULL, 0, "%d", ace->u.e_id);
			}
			if (a >= align)
				align = a + 1;
		}
	}

	buffer = alloc_string_buffer(128);
	if (!buffer)
		return NULL;

	write_acl_flags(buffer, acl->a_flags, fmt);
	if (fmt & RICHACL_TEXT_SHOW_MASKS) {
		unsigned int allowed = 0;

		fmt2 = fmt;
		richacl_for_each_entry(ace, acl) {
			if (richace_is_inherit_only(ace))
				continue;

			if (richace_is_allow(ace))
				allowed |= ace->e_mask;

			if (ace->e_flags & ACE4_FILE_INHERIT_ACE)
				fmt2 |= RICHACL_TEXT_FILE_CONTEXT;
			if (ace->e_flags & ACE4_DIRECTORY_INHERIT_ACE)
				fmt2 |= RICHACL_TEXT_DIRECTORY_CONTEXT;
		}

		if (!(fmt & RICHACL_TEXT_SIMPLIFY))
			allowed = ~0;

		buffer_sprintf(buffer, "%*s:", align, "owner");
		write_mask(buffer, acl->a_owner_mask & allowed, fmt2);
		buffer_sprintf(buffer, "::mask\n");
		buffer_sprintf(buffer, "%*s:", align, "group");
		write_mask(buffer, acl->a_group_mask & allowed, fmt2);
		buffer_sprintf(buffer, "::mask\n");
		buffer_sprintf(buffer, "%*s:", align, "other");
		write_mask(buffer, acl->a_other_mask & allowed, fmt2);
		buffer_sprintf(buffer, "::mask\n");
	}

	richacl_for_each_entry(ace, acl) {
		write_identifier(buffer, ace, align, fmt);
		buffer_sprintf(buffer, ":");

		fmt2 = fmt;
		if (ace->e_flags & ACE4_INHERIT_ONLY_ACE)
			fmt2 &= ~(RICHACL_TEXT_FILE_CONTEXT |
				  RICHACL_TEXT_DIRECTORY_CONTEXT);
		if (ace->e_flags & ACE4_FILE_INHERIT_ACE)
			fmt2 |= RICHACL_TEXT_FILE_CONTEXT;
		if (ace->e_flags & ACE4_DIRECTORY_INHERIT_ACE)
			fmt2 |= RICHACL_TEXT_DIRECTORY_CONTEXT;

		write_mask(buffer, ace->e_mask, fmt2);
		buffer_sprintf(buffer, ":");
		write_ace_flags(buffer, ace->e_flags, fmt2);
		buffer_sprintf(buffer, ":");
		write_type(buffer, ace->e_type);
		buffer_sprintf(buffer, "\n");
	}

	if (string_buffer_okay(buffer)) {
		char *str = realloc(buffer->buffer, buffer->offset + 1);
		if (str)
			return str;
	}

	free_string_buffer(buffer);
	errno = ENOMEM;
	return NULL;
}

static int acl_flags_from_text(const char *str, struct richacl *acl,
			       void (*error)(const char *, ...))
{
	char *dup, *end;

	end = alloca(strlen(str) + 1);
	strcpy(end, str);

	acl->a_flags = 0;
	while ((dup = end)) {
		char *c;
		unsigned long l;
		int i;

		while (*dup == '/')
			dup++;
		end = strchr(dup, '/');
		if (end)
			*end++ = 0;
		if (!*dup)
			break;

		l = strtoul(str, &c, 0);
		if (*c == 0) {
			acl->a_flags |= l;
			continue;
		}

		/* Recognize flag mnemonics */
		for (i = 0; i < ARRAY_SIZE(acl_flag_bits); i++) {
			if (!strcasecmp(dup, acl_flag_bits[i].a_name)) {
				acl->a_flags |= acl_flag_bits[i].a_flag;
				break;
			}
		}
		if (i != ARRAY_SIZE(acl_flag_bits))
			continue;

		/* Recognize single-character flags */
		for (c = dup; *c; c++) {
			for (i = 0; i < ARRAY_SIZE(acl_flag_bits); i++) {
				if (*c == acl_flag_bits[i].a_char) {
					acl->a_flags |= acl_flag_bits[i].a_flag;
					break;
				}
			}
			if (i != ARRAY_SIZE(acl_flag_bits))
				continue;

			error("Invalid acl flag `%s'\n", c);
			return -1;
		}
	}

	return 0;
}

static int identifier_from_text(const char *str, struct richace *ace,
				void (*error)(const char *, ...))
{
	char *c;
	unsigned long l;

	c = strchr(str, '@');
	if (c) {
		char *dup;

		if (c[1]) {
			error("Domain name not supported in `%s'\n", str);
			goto fail;
		}

		/* Ignore case in special identifiers. */
		dup = alloca(strlen(str) + 1);
		strcpy(dup, str);
		for (c = dup; *c; c++)
			*c = toupper(*c);

		if (richace_set_who(ace, dup)) {
			error("Special user `%s' not supported\n", str);
			goto fail;
		}
		return 0;
	}
	l = strtoul(str, &c, 0);
	if (*c == 0) {
		ace->u.e_id = l;
		return 0;
	}
	if (ace->e_flags & ACE4_IDENTIFIER_GROUP) {
		struct group *group = getgrnam(str);

		if (!group) {
			error("Group `%s' does not exist\n", str);
			goto fail;
		}
		ace->u.e_id = group->gr_gid;
		return 0;
	} else {
		struct passwd *passwd = getpwnam(str);

		if (!passwd) {
			error("User `%s' does not exist\n", str);
			goto fail;
		}
		ace->u.e_id = passwd->pw_uid;
		return 0;
	}
fail:
	return -1;
}

static int type_from_text(const char *str, struct richace *ace,
			  void (*error)(const char *, ...))
{
	char *c;
	int i;
	unsigned long l;

	l = strtoul(str, &c, 0);
	if (*c == 0) {
		ace->e_type = l;
		return 0;
	}

	/* Recognize type mnemonic */
	for (i = 0; i < ARRAY_SIZE(type_values); i++) {
		if (!strcasecmp(str, type_values[i].e_name)) {
			ace->e_type = type_values[i].e_type;
			return 0;
		}
	}
	error("Invalid entry type `%s'\n", str);
	return -1;
}

static int ace_flags_from_text(const char *str, struct richace *ace,
			       void (*error)(const char *, ...))
{
	char *dup, *end;

	end = alloca(strlen(str) + 1);
	strcpy(end, str);

	ace->e_flags = 0;
	while ((dup = end)) {
		char *c;
		unsigned long l;
		int i;

		while (*dup == '/')
			dup++;
		end = strchr(dup, '/');
		if (end)
			*end++ = 0;
		if (!*dup)
			break;

		l = strtoul(str, &c, 0);
		if (*c == 0) {
			ace->e_flags |= l;
			continue;
		}

		/* Recognize flag mnemonics */
		for (i = 0; i < ARRAY_SIZE(ace_flag_bits); i++) {
			if (!strcasecmp(dup, ace_flag_bits[i].e_name)) {
				ace->e_flags |= ace_flag_bits[i].e_flag;
				break;
			}
		}
		if (i != ARRAY_SIZE(ace_flag_bits))
			continue;

		/* Recognize single-character flags */
		for (c = dup; *c; c++) {
			for (i = 0; i < ARRAY_SIZE(ace_flag_bits); i++) {
				if (*c == ace_flag_bits[i].e_char) {
					ace->e_flags |= ace_flag_bits[i].e_flag;
					break;
				}
			}
			if (i != ARRAY_SIZE(ace_flag_bits))
				continue;

			error("Invalid entry flag `%s'\n", c);
			return -1;
		}
	}

	return 0;
}

static int mask_from_text(const char *str, unsigned int *mask,
			  void (*error)(const char *, ...))
{
	char *dup, *end;

	end = alloca(strlen(str) + 1);
	strcpy(end, str);

	*mask = 0;
	while ((dup = end)) {
		char *c;
		unsigned long l;
		int i;

		while (*dup == '/')
			dup++;
		end = strchr(dup, '/');
		if (end)
			*end++ = 0;
		if (!*dup)
			break;

		l = strtoul(dup, &c, 0);
		if (*c == 0) {
			*mask |= l;
			continue;
		}

		/* Recognize mask mnemonics */
		for (i = 0; i < ARRAY_SIZE(mask_flags); i++) {
			if (!strcasecmp(dup, mask_flags[i].e_name)) {
				*mask |= mask_flags[i].e_mask;
				break;
			}
		}
		if (i != ARRAY_SIZE(mask_flags))
			continue;

		/* Recognize single-character masks */
		for (c = dup; *c; c++) {
			if (*c == '-')
				continue;
			for (i = 0; i < ARRAY_SIZE(mask_flags); i++) {
				if (*c == mask_flags[i].e_char) {
					*mask |= mask_flags[i].e_mask;
					break;
				}
			}
			if (i != ARRAY_SIZE(mask_flags))
				continue;

			error("Invalid access mask `%s'\n", dup);
			return -1;
		}
	}

	return 0;
}

struct richacl *richacl_from_text(const char *str, int *pflags,
				  void (*error)(const char *, ...))
{
	char *who_str = NULL, *mask_str = NULL, *flags_str = NULL,
	     *type_str = NULL;
	struct richacl *acl;
	int flags = 0;

	acl = richacl_alloc(0);
	if (!acl)
		return NULL;

	while (*str) {
		struct richacl *acl2;
		struct richace *ace;
		unsigned int mask;
		const char *entry, *c;

		while (isspace(*str) || *str == ',')
			str++;
		if (!*str)
			break;

		entry = str;
		c = strchr(str, ':');
		if (!c)
			goto fail_syntax;
		who_str = strndup(str, c - str);
		if (!who_str)
			goto fail;
		str = c + 1;

		if (!strcasecmp(who_str, "FLAGS")) {
			for (c = str; *c; c++) {
				if (*c == ':' || *c == ',' || isspace(*c))
					break;
			}
			mask_str = strndup(str, c - str);
			if (!mask_str)
				goto fail;
			if (*c != ':') {
				if (acl_flags_from_text(mask_str, acl, error))
					goto fail_einval;
				flags |= RICHACL_TEXT_FLAGS;
				str = c;
				goto free_mask_str;
			} else {
				free(mask_str);
				mask_str = NULL;
			}
		}

		c = strchr(str, ':');
		if (!c)
			goto fail_syntax;
		mask_str = strndup(str, c - str);
		if (!mask_str)
			goto fail;
		str = c + 1;

		c = strchr(str, ':');
		if (!c)
			goto fail_syntax;
		flags_str = strndup(str, c - str);
		if (!flags_str)
			goto fail;
		str = c + 1;

		for (c = str; *c; c++) {
			if (*c == ',' || isspace(*c))
				break;
		}
		type_str = strndup(str, c - str);
		if (!type_str)
			goto fail;
		str = c;

		if (mask_from_text(mask_str, &mask, error))
			goto fail_einval;
		if (!strcasecmp(type_str, "MASK")) {
			if (!strcasecmp(who_str, "OWNER")) {
				acl->a_owner_mask = mask;
				flags |= RICHACL_TEXT_OWNER_MASK;
			} else if (!strcasecmp(who_str, "GROUP")) {
				acl->a_group_mask = mask;
				flags |= RICHACL_TEXT_GROUP_MASK;
			} else if (!strcasecmp(who_str, "OTHER")) {
				acl->a_other_mask = mask;
				flags |= RICHACL_TEXT_OTHER_MASK;
			} else {
				error("Invalid file mask `%s'\n",
				      who_str);
				goto fail_einval;
			}
		} else {
			size_t size = sizeof(struct richacl) + (acl->a_count
				      + 1) * sizeof(struct richace);
			acl2 = realloc(acl, size);
			if (!acl2)
				goto fail;
			acl = acl2;
			memset(acl->a_entries + acl->a_count, 0,
			       sizeof(struct richace));
			acl->a_count++;

			ace = acl->a_entries + acl->a_count - 1;
			ace->e_mask = mask;
			if (ace_flags_from_text(flags_str, ace, error))
				goto fail_einval;
			if (identifier_from_text(who_str, ace, error))
				goto fail_einval;
			if (type_from_text(type_str, ace, error))
				goto fail_einval;
		}

		free(type_str);
		type_str = NULL;
		free(flags_str);
		flags_str = NULL;
	free_mask_str:
		free(mask_str);
		mask_str = NULL;
		free(who_str);
		who_str = NULL;
		continue;

	fail_syntax:
		for (c = entry; *c && !(isspace(*c) || *c == ','); c++)
			;
		error("Invalid entry `%.*s'\n", c - entry, entry);
		goto fail_einval;
	}

	if (pflags)
		*pflags = flags;
	return acl;

fail_einval:
	errno = EINVAL;

fail:
	free(type_str);
	free(flags_str);
	free(mask_str);
	free(who_str);
	richacl_free(acl);
	return NULL;
}

int richace_set_who(struct richace *ace, const char *who)
{
	if (!strcmp(who, richace_owner_who))
		who = richace_owner_who;
	else if (!strcmp(who, richace_group_who))
		who = richace_group_who;
	else if (!strcmp(who, richace_everyone_who))
		who = richace_everyone_who;
	else
		return -1;

	ace->u.e_who = who;
	ace->e_flags |= ACE4_SPECIAL_WHO;
	/*
	 * Also clear the ACE4_IDENTIFIER_GROUP flag for ACEs with a special
	 * who value: richace_is_same_identifier() relies on that.
	 */
	ace->e_flags &= ~ACE4_IDENTIFIER_GROUP;
	return 0;
}

void richace_set_uid(struct richace *ace, uid_t uid)
{
	ace->u.e_id = uid;
	ace->e_flags &= ~(ACE4_SPECIAL_WHO | ACE4_IDENTIFIER_GROUP);
}

void richace_set_gid(struct richace *ace, gid_t gid)
{
	ace->u.e_id = gid;
	ace->e_flags |= ACE4_IDENTIFIER_GROUP;
	ace->e_flags &= ~ACE4_SPECIAL_WHO;
}

void richace_copy(struct richace *dst, const struct richace *src)
{
	memcpy(dst, src, sizeof(struct richace));
}

/**
 * richacl_mode_to_mask  - compute a file mask from the lowest three mode bits
 *
 * See richacl_masks_to_mode().
 */
static unsigned int richacl_mode_to_mask(mode_t mode)
{
	unsigned int mask = ACE4_POSIX_ALWAYS_ALLOWED;

	if (mode & S_IROTH)
		mask |= ACE4_POSIX_MODE_READ;
	if (mode & S_IWOTH)
		mask |= ACE4_POSIX_MODE_WRITE;
	if (mode & S_IXOTH)
		mask |= ACE4_POSIX_MODE_EXEC;

	return mask;
}

/**
 * richacl_from_mode  -  create an acl which corresponds to @mode
 * @mode:       file mode including the file type
 */
struct richacl *richacl_from_mode(mode_t mode)
{
	struct richacl *acl;
	struct richace *ace;

	acl = richacl_alloc(1);
	if (!acl)
		return NULL;
	ace = acl->a_entries;

	acl->a_owner_mask = richacl_mode_to_mask(mode >> 6);
	acl->a_group_mask = richacl_mode_to_mask(mode >> 3);
	acl->a_other_mask = richacl_mode_to_mask(mode);

	ace->e_type = ACE4_ACCESS_ALLOWED_ACE_TYPE;
	ace->e_flags = ACE4_SPECIAL_WHO;
	ace->e_mask = ACE4_POSIX_MODE_ALL;
	/* ACE4_DELETE_CHILD is meaningless for non-directories. */
	if (!S_ISDIR(mode))
		ace->e_mask &= ~ACE4_DELETE_CHILD;
	ace->u.e_who = richace_everyone_who;

	return acl;
}

int richace_is_unix_id(const struct richace *ace)
{
	return !(ace->e_flags & ACE4_SPECIAL_WHO);
}

static int in_groups(gid_t group, gid_t groups[], int n_groups)
{
	int n;

	for (n = 0; n < n_groups; n++)
		if (group == groups[n])
			return 1;
	return 0;
}

int richacl_access(const char *file, const struct stat *st, uid_t user,
		   const gid_t *const_groups, int n_groups)
{
	const struct richacl *acl;
	struct stat local_st;
	const struct richace *ace;
	unsigned int file_mask, mask = ACE4_VALID_MASK, denied = 0;
	int in_owning_group;
	int in_owner_or_group_class;
	gid_t *groups = NULL;

	if (!st) {
		if (stat(file, &local_st) != 0)
			return -1;
		st = &local_st;
	}

	acl = richacl_get_file(file);
	if (!acl) {
		if (errno == ENODATA || errno == ENOTSUP || errno == ENOSYS) {
			acl = richacl_from_mode(st->st_mode);
			if (!acl)
				return -1;
		} else
			return -1;
	}

	if (n_groups < 0) {
		n_groups = getgroups(0, NULL);
		if (n_groups < 0)
			return -1;
		groups = malloc(sizeof(gid_t) * (n_groups + 1));
		if (!groups)
			return -1;
		groups[0] = getegid();
		if (getgroups(n_groups, groups + 1) < 0) {
			free(groups);
			return -1;
		}
	} else
		groups = (gid_t *)const_groups;  /* cast away const */

	in_owning_group = in_groups(st->st_gid, groups, n_groups);
	in_owner_or_group_class = in_owning_group;

	/*
	 * A process is
	 *   - in the owner file class if it owns the file,
	 *   - in the group file class if it is in the file's owning group or
	 *     it matches any of the user or group entries, and
	 *   - in the other file class otherwise.
	 */

	richacl_for_each_entry(ace, acl) {
		unsigned int ace_mask = ace->e_mask;

		if (richace_is_inherit_only(ace))
			continue;
		if (richace_is_owner(ace)) {
			if (user != st->st_uid)
				continue;
			goto is_owner;
		} else if (richace_is_group(ace)) {
			if (!in_owning_group)
				continue;
		} else if (richace_is_unix_id(ace)) {
			if (ace->e_flags & ACE4_IDENTIFIER_GROUP) {
				if (!in_groups(ace->u.e_id, groups, n_groups))
					continue;
			} else {
				if (user != ace->u.e_id)
					continue;
			}
		} else
			goto is_everyone;

		/*
		 * Apply the group file mask to entries other than OWNER@ and
		 * EVERYONE@. This is not required for correct access checking
		 * but ensures that we grant the same permissions as the acl
		 * computed by richacl_apply_masks() would grant.
		 */
		if (richace_is_allow(ace))
			ace_mask &= acl->a_group_mask;

is_owner:
		/* The process is in the owner or group file class. */
		in_owner_or_group_class = 1;

is_everyone:
		/* Check which mask flags the ACE allows or denies. */
		if (richace_is_deny(ace))
			denied |= ace_mask & mask;
		mask &= ~ace_mask;
		if (!mask)
			break;
	}
	denied |= mask;

	/*
	 * Figure out which file mask applies.
	 */
	if (user == st->st_uid) {
		file_mask = acl->a_owner_mask |
			    (ACE4_WRITE_ATTRIBUTES | ACE4_WRITE_ACL);
		denied &= ~(ACE4_WRITE_ATTRIBUTES | ACE4_WRITE_ACL);
	} else if (in_owner_or_group_class)
		file_mask = acl->a_group_mask;
	else
		file_mask = acl->a_other_mask;
	/* ACE4_DELETE_CHILD is meaningless for non-directories. */
	if (!S_ISDIR(st->st_mode))
		file_mask &= ~ACE4_DELETE_CHILD;

	if (groups != const_groups)
		free(groups);

	return ACE4_POSIX_ALWAYS_ALLOWED | (file_mask & ~denied);
}

char *richacl_mask_to_text(unsigned int mask, int fmt)
{
	struct string_buffer *buffer;

	buffer = alloc_string_buffer(16);
	if (!buffer)
		return NULL;
	write_mask(buffer, mask, fmt);

	if (string_buffer_okay(buffer)) {
		char *str = realloc(buffer->buffer, buffer->offset + 1);
		if (str)
			return str;
	}

	free_string_buffer(buffer);
	errno = ENOMEM;
	return NULL;
}

/**
 * richacl_mask_to_mode  -  compute the file permission bits which correspond to @mask
 * @mask:	%ACE4_* permission mask
 *
 * See richacl_masks_to_mode().
 */
static int
richacl_mask_to_mode(unsigned int mask)
{
	int mode = 0;

	if (mask & ACE4_POSIX_MODE_READ)
		mode |= MAY_READ;
	if (mask & ACE4_POSIX_MODE_WRITE)
		mode |= MAY_WRITE;
	if (mask & ACE4_POSIX_MODE_EXEC)
		mode |= MAY_EXEC;

	return mode;
}

/**
 * richacl_masks_to_mode  -  compute the file permission bits from the file masks
 *
 * When the file permission bits of a file are set with chmod(), this specifies
 * the maximum permissions that processes will get.  All permissions beyond
 * that are removed from the file masks, and become ineffective.
 *
 * Conversely, when setting a richacl, we set the file permission bits to
 * indicate maximum permissions: for example, we set the Write permission when
 * a mask contains ACE4_APPEND_DATA even if it does not also contain
 * ACE4_WRITE_DATA.
 *
 * Permissions which are not in ACE4_POSIX_MODE_READ, ACE4_POSIX_MODE_WRITE, or
 * ACE4_POSIX_MODE_EXEC cannot be represented in the file permission bits.
 * Those permissions can still be effective, but only if the masks were set
 * explicitly (for example, by setting the richacl xattr), and not for new
 * files or after a chmod().
 */
int
richacl_masks_to_mode(const struct richacl *acl)
{
	return richacl_mask_to_mode(acl->a_owner_mask) << 6 |
	       richacl_mask_to_mode(acl->a_group_mask) << 3 |
	       richacl_mask_to_mode(acl->a_other_mask);
}

/**
 * richacl_equiv_mode  -  determine if @acl is equivalent to a file mode
 * @mode_p:	the file mode
 *
 * The file type in @mode_p must be set when calling richacl_equiv_mode().
 * Returns with 0 if @acl is equivalent to a file mode; in that case, the
 * file permission bits in @mode_p are set to the mode equivalent to @acl.
 */
int
richacl_equiv_mode(const struct richacl *acl, mode_t *mode_p)
{
	const struct richace *ace = acl->a_entries;
	unsigned int x = ~ACE4_POSIX_ALWAYS_ALLOWED;  /* mask flags we care about */
	mode_t mode;

	if (acl->a_count != 1 ||
	    acl->a_flags ||
	    !richace_is_everyone(ace) ||
	    !richace_is_allow(ace) ||
	    ace->e_flags & ~ACE4_SPECIAL_WHO)
		return -1;

	/* ACE4_DELETE_CHILD is meaningless for non-directories. */
	if (!S_ISDIR(*mode_p))
		x &= ~ACE4_DELETE_CHILD;

	if ((ace->e_mask & x) != (ACE4_POSIX_MODE_ALL & x))
		return -1;

	mode = richacl_masks_to_mode(acl);
	if ((acl->a_owner_mask & x) != (richacl_mode_to_mask(mode >> 6) & x) ||
	    (acl->a_group_mask & x) != (richacl_mode_to_mask(mode >> 3) & x) ||
	    (acl->a_other_mask & x) != (richacl_mode_to_mask(mode) & x))
		return -1;

	*mode_p = (*mode_p & ~S_IRWXUGO) | mode;
	return 0;
}
